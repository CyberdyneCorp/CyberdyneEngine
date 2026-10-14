// SPDX-License-Identifier: MIT
#pragma once
// The engine side of the editor's GI diagnostics: `illumination.report`. Issue #35.
//
// The editor shows what CyberGI made of the authored world — the settings it runs at, the tier its
// rays took, its convergence and cost per stage, every emitter with the class the measurement or an
// author's rule gave it, every object's illumination class — and, for a point the author clicked
// in the viewport, `IlluminationSystem::explain`: which sources lit it, how far the region has
// converged, how many probes answered and whether any of them is a leak suspect, and why it is the
// colour it is. Every number is the GI's own; the editor computes none of them.
//
// The host supplies the GI (`IlluminationRuntime::preview`, an `IlluminationPreview` it keeps in
// step with the document) and turns a pixel of a published frame into a ray.
//
// REQUEST (schema 1), little-endian:
//   u32 format = 1, u8 pick, and when `pick` is 1: u32 viewport, u64 frame, f32 x, f32 y (pixels).
// REPLY:
//   u32 format = 1,
//   settings  text mode, text quality, text tier, u8 hardware ray tracing, f32 budget ms,
//   status    text world tier, f32 convergence, u32 cards, u32 probes, u32 volumes, u32 lights,
//             u64 rebuilds, u64 applications,
//   profile   u64 frame, f32 allocation ms, f32 measured ms, u32 stage count, per stage
//             (text name, f64 ms), u32 probes updated, u32 surface pages updated, u32 screen rays,
//             u32 software rays, u32 hardware rays, f32 surface cache hit rate,
//             f32 radiance cache hit rate,
//   emissive  u32 emitters, f32 total, cached, promoted, demoted and disabled power, u32 count,
//             per emitter (u64 node, text material, text class, text rule, u32 cards, f32 area m^2,
//             f32 power),
//   objects   u32 count, per object (u64 node, text class, u8 overridden),
//   explained u8, and when 1: vec3 point, vec3 normal, text cause, vec3 diffuse, f32 confidence,
//             text dominant source, u32 probes contributing, u32 probes in reach, u32 leak
//             suspects, f32 leak weight, text world tier, f32 convergence, u32 source count, per
//             source (text name, vec3 radiance, f32 confidence, f32 weight share, u8 excluded).
// A text is a u32 byte count and UTF-8; a vec3 three f32.

#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/allocator.h>
#include <cy/editor/illumination_preview.h>

namespace cy::editor {

/// The request and reply format `illumination.report` speaks.
inline constexpr u32 kIlluminationReportFormat = 1;
/// The `capabilities.get` feature bit an illumination service with a host seam sets.
inline constexpr u64 kIlluminationFeature = 0x200U;

/// The runtime host's side: the GI it keeps in step with the document, and its frames' rays.
class IlluminationRuntime {
public:
    virtual ~IlluminationRuntime() = default;

    /// The authored world's GI, brought up to date with the document; null with no world open.
    [[nodiscard]] virtual IlluminationPreview* preview() noexcept = 0;
    /// The world-space ray under pixel (`x`, `y`) of `viewport`'s frame `frame`.
    [[nodiscard]] virtual Expected<Ray, Error> pick_ray(u32 viewport, u64 frame, f32 x,
                                                        f32 y) noexcept = 0;
};

/// Encode the reply for `report`.
[[nodiscard]] Status encode_illumination_report(const IlluminationReport& report,
                                                Array<u8>& out) noexcept;

/// Engine-owned illumination backend: one pending request per session, answered on the next poll
/// with exactly one terminal event.
class IlluminationService final : public abi::EditorServiceBackend {
public:
    /// Without a `runtime`, `illumination.report` fails with `illumination.report.unavailable` and
    /// `capabilities.get` lists nothing but itself.
    explicit IlluminationService(Allocator& allocator,
                                 IlluminationRuntime* runtime = nullptr) noexcept
        : allocator_(&allocator), runtime_(runtime) {}

    [[nodiscard]] CyResult open(CyServiceSession* out_session) noexcept override;
    void close(CyServiceSession session) noexcept override;
    [[nodiscard]] CyResult submit(CyServiceSession session,
                                  const CyServiceRequest& request) noexcept override;
    [[nodiscard]] CyResult cancel(CyServiceSession session, u64 request_id) noexcept override;
    [[nodiscard]] CyResult poll(CyServiceSession session, CyServiceEvent& out_event,
                                bool& out_has_event) noexcept override;

private:
    Allocator* allocator_;
    IlluminationRuntime* runtime_;
};

}  // namespace cy::editor
