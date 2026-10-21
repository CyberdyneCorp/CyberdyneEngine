// SPDX-License-Identifier: MIT
#pragma once
// The one interface every upscaler sits behind, and the seam a vendor upscaler plugs into.
// `rendering-post-processing` — "Temporal upscaling":
//
//   "Upscalers SHALL be pluggable behind one interface taking colour, depth, motion vectors,
//    exposure, and jitter — the latter three supplied by the temporal framework. The engine SHALL
//    ship a built-in temporal upscaler and SHALL define the integration seams for vendor upscalers
//    (FSR, DLSS, XeSS, MetalFX) as optional modules ... Public renderer interfaces SHALL NOT depend
//    on vendor types."
//
// ================================================================================================
// WHAT THE INTERFACE IS
// ================================================================================================
//
// The frame's `Temporal` stage is declared by a producer through `FrameStageDeclaration`, and an
// upscaler is that producer. It is handed `ScreenSpaceStageInputs`: the colour (`source`), the
// depth, the motion vectors and the pre-translucency colour at the RENDER extent
// (`render_width`, `render_height`), and the targets it writes at the OUTPUT extent (`width`,
// `height`). The jitter, the matrices and history validity come from the `TemporalFramework` it is
// bound to — never from the caller, which is what "supplied by the temporal framework" means — and
// the exposure from its settings.
//
// A vendor module implements `Upscaler` and adds itself to an `UpscalerRegistry`. Nothing in this
// header, the frame or the assembly names a vendor type: the enumerators below are the SLOTS a
// vendor module fills, and an empty slot is the built-in upscaler.
//
// ================================================================================================
// "VENDOR UPSCALER ABSENT"
// ================================================================================================
//
// `select()` returns the preferred family when a module for it was added AND says it can run on
// this device, and the built-in upscaler otherwise. The frame cannot tell the difference — the same
// stage, the same inputs, the same targets — which is the requirement's "with no content changes".

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/upscale/upscale.h>

namespace cy::rendering::upscale {

/// Who implements an upscaler. Slots, not types.
enum class UpscalerFamily : u8 {
    BuiltIn = 0,
    /// AMD FidelityFX Super Resolution 2 or later.
    Fsr,
    /// NVIDIA Deep Learning Super Sampling.
    Dlss,
    /// Intel Xe Super Sampling.
    Xess,
    /// Apple MetalFX temporal scaling.
    MetalFx,
    Count,
};

inline constexpr u32 kUpscalerFamilyCount = static_cast<u32>(UpscalerFamily::Count);

[[nodiscard]] const char* upscaler_family_name(UpscalerFamily family) noexcept;

/// The interface. An implementation is a frame stage producer with a name and an availability.
class Upscaler {
public:
    Upscaler() = default;
    virtual ~Upscaler() = default;
    Upscaler(const Upscaler&) = delete;
    Upscaler& operator=(const Upscaler&) = delete;
    Upscaler(Upscaler&&) = delete;
    Upscaler& operator=(Upscaler&&) = delete;

    [[nodiscard]] virtual UpscalerFamily family() const noexcept = 0;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    /// Whether it can run on `device`. Null asks whether it could run at all — a module whose
    /// runtime library is missing answers false either way.
    [[nodiscard]] virtual bool available(const rhi::Device* device) const noexcept = 0;
    /// Declare the stage's passes into `graph`. Returns the first, or `kInvalidPass` to refuse —
    /// which fails the frame rather than leaving it without the stage.
    [[nodiscard]] virtual PassId declare(RenderGraph& graph,
                                         const ScreenSpaceStageInputs& inputs) noexcept = 0;
    /// The frame that declared the stage was executed and its fence passed.
    virtual void executed() noexcept = 0;
};

/// The `FrameStageDeclaration` that hands the frame's `Temporal` stage to `upscaler`, with its
/// `executed` hook set. What a caller puts in `FrameSinks::temporal`.
[[nodiscard]] FrameStageDeclaration stage_of(Upscaler& upscaler) noexcept;

/// The upscalers a renderer can choose from: the built-in one, always, and whatever vendor modules
/// were added.
class UpscalerRegistry {
public:
    explicit UpscalerRegistry(Upscaler& built_in) noexcept : built_in_(&built_in) {}

    /// Add a vendor module. Refuses the built-in family, a family already added, and an upscaler
    /// whose family is out of range.
    [[nodiscard]] Status add(Upscaler& vendor) noexcept;

    /// The preferred family when it was added and is available on `device`, and the built-in
    /// upscaler otherwise.
    [[nodiscard]] Upscaler& select(UpscalerFamily preferred,
                                   const rhi::Device* device) const noexcept;

    [[nodiscard]] bool has(UpscalerFamily family) const noexcept;
    [[nodiscard]] Upscaler& built_in() const noexcept { return *built_in_; }

private:
    Upscaler* built_in_ = nullptr;
    Upscaler* vendors_[kUpscalerFamilyCount] = {};
};

}  // namespace cy::rendering::upscale
