// SPDX-License-Identifier: MIT
#pragma once
// Irradiance volumes: a regular grid of spherical-harmonic probes captured from the scene and the
// sky, sampled trilinearly with a visibility term. The first slice of global illumination the
// forward frame shades with.
//
// `rendering-global-illumination` — "Irradiance volumes and light probes", and the `Probe` mode of
// "GI strategy layers".
//
// ================================================================================================
// WHAT THIS IS, AND WHAT IT IS NOT
// ================================================================================================
//
// IT IS: a box of probes on a regular grid, each storing the radiance arriving at it as SH L1 (four
// coefficients per channel, the radiance cache's own `ProbeEncoding::SphericalHarmonicsL1`, encoded
// and decoded by the same functions). A probe is captured by tracing a fixed Fibonacci set of rays
// through the SAME seams the radiance cache and the bake gather through — a `SceneTracer` for where
// a ray lands, a `RadianceLookup` for what leaves the surface there, and the `SkyTerm` where it
// escapes. So one bounce of scene light, the sky, and — when the lookup feeds the volume back into
// itself — further bounces over successive captures.
//
// A query takes the eight probes around it, weighted trilinearly and then by three factors that
// together are the visibility term the requirement asks for:
//
//   validity    a probe whose rays mostly hit the INSIDE of geometry is inside a wall; weight zero.
//   backface    a probe behind the queried surface's own plane is attenuated (never zeroed), so a
//               floor does not take the light a probe under it saw.
//   distance    each probe records how far the world is along the six axes, which bounds a box
//               of free space around it; a query outside that box on any axis is behind
//               something and weighs zero.
//
// IT IS NOT full dynamic GI. Nothing here re-captures by itself when a light moves; a caller that
// changed the lighting invalidates the region (or runs the `Amortised` policy, which refreshes the
// stalest probes within a budget every update). Probes are placed on a regular grid only: adaptive
// subdivision and hand placement are NOT implemented. There is no specular term, and there is no
// per-pixel occlusion beyond what the six axis distances resolve — a thin wall between two probes
// that the axis rays miss still leaks. The capture treats every surface as Lambertian.
//
// ================================================================================================
// THE UPDATE POLICY IS STATED, NOT IMPLIED
// ================================================================================================
//
//   * `configure()` leaves every probe queued and invalid: nothing is lit by a probe that was never
//     captured, which is the frame's flat ambient under the volume's coverage rule.
//   * `capture_all()` is the bake: every probe, one pass, staged and committed together so the
//     result does not depend on the order probes were visited in.
//   * `invalidate(region)` queues the probes whose cells touch `region`, first in first out.
//   * `update()` captures at most `probes_per_update` queued probes. Under `Amortised` the budget
//     left over refreshes the stalest probes round-robin, so every probe is refreshed at least once
//     every `ceil(probe_count / probes_per_update)` updates. Under `OnInvalidation` it does nothing
//     more, and a static scene costs nothing after its bake.
//
// ================================================================================================
// THE FRAME READS WHAT `pack_texels` WRITES, AND `ambient()` IS WHAT THE FRAME COMPUTES
// ================================================================================================
//
// `cy/frame.slang`'s `probeVolumeRadiance` is a transcription of `sample()` below, reading the
// texels `pack_texels` lays out. `ambient()` is the whole of the frame's ambient term with the
// volume attached — the flat ambient where the volume does not reach, the volume where it does,
// and a blend over one cell at its boundary — so a host test can state what a pixel should be.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/scene.h>

namespace cy::rendering::gi {

/// When probes are re-captured. See the header comment.
enum class VolumeUpdatePolicy : u8 {
    /// Only probes that were invalidated. A static scene costs nothing after its bake.
    OnInvalidation = 0,
    /// Invalidated probes first, then the stalest probes round-robin within the same budget.
    Amortised,
};

struct IrradianceVolumeSettings {
    /// The world position of probe (0, 0, 0). Probe (i, j, k) is at `origin + spacing * (i, j, k)`.
    Vec3 origin{0.0F, 0.0F, 0.0F};
    f32 spacing_metres = 1.0F;
    /// Probes along x, y and z. Each at least one.
    u32 count_x = 4;
    u32 count_y = 4;
    u32 count_z = 4;
    /// Rays per probe capture, over a fixed Fibonacci sphere: the same set every capture, so a
    /// capture is a number rather than a draw.
    u32 rays_per_probe = 256;
    f32 max_ray_distance_metres = 40.0F;
    /// How far along the surface normal a query is moved before it is located in the grid. Keeps a
    /// surface lying exactly on a probe plane from flickering between the cells on either side.
    f32 normal_offset_metres = 0.1F;
    /// The soft edge of the distance visibility test, in metres.
    f32 visibility_slack_metres = 0.25F;
    /// The fraction of a probe's rays that may hit the inside of geometry before the probe is
    /// treated as being inside it and given zero weight.
    f32 inside_fraction = 0.25F;
    VolumeUpdatePolicy policy = VolumeUpdatePolicy::OnInvalidation;
    /// Probes one `update()` captures.
    u32 probes_per_update = 16;
};

/// What one capture reads. `tracer` null means "nothing but sky", which is honest for an open
/// scene and is what a probe with no world to see would record anyway.
struct VolumeCaptureContext {
    const SceneTracer* tracer = nullptr;
    const RadianceLookup* radiance = nullptr;
    SkyTerm sky{};
    /// The update count, recorded on each probe so the stalest can be found.
    u64 frame = 0;
};

/// One probe.
struct VolumeProbe {
    Vec3 position{0.0F, 0.0F, 0.0F};
    /// SH L1, `payload[coefficient * 3 + channel]`, in the radiance cache's encoding.
    f32 payload[12] = {};
    /// World distance along +x, -x, +y, -y, +z, -z, in metres.
    f32 axis_distance[6] = {};
    /// 1 for a probe in open space, 0 for a probe inside geometry or never captured.
    f32 validity = 0.0F;
    u64 captured_frame = 0;
    bool captured = false;
    bool queued = false;
};

struct VolumeUpdateReport {
    u32 probes_captured = 0;
    /// Queued probes left after this update.
    u32 queue_depth = 0;
    u32 invalid_probes = 0;
    u64 rays = 0;
};

/// What a query found.
struct VolumeSample {
    /// The visibility-weighted mean of the surrounding probes' irradiance, as a radiance (the mean
    /// over the cosine lobe, `decode_payload`'s unit).
    Vec3 radiance{0.0F, 0.0F, 0.0F};
    /// 1 inside the volume, falling to 0 over one cell outside it.
    f32 coverage = 0.0F;
    /// The weight the query found. Zero means every surrounding probe was invalid or behind a wall.
    f32 weight = 0.0F;
};

/// The texels one probe occupies in the packed texture.
inline constexpr u32 kVolumeTexelsPerProbe = 5;

/// The packed texture's layout and the scale its coefficients were divided by.
struct VolumeTextureLayout {
    u32 width = 0;
    u32 height = 0;
    /// Every SH coefficient is stored divided by this, so the largest fits a half float with room.
    f32 coefficient_scale = 1.0F;
};

class IrradianceVolume : public IndirectSource {
public:
    IrradianceVolume() noexcept;
    ~IrradianceVolume() override;

    IrradianceVolume(const IrradianceVolume&) = delete;
    IrradianceVolume(IrradianceVolume&&) = delete;
    IrradianceVolume& operator=(const IrradianceVolume&) = delete;
    IrradianceVolume& operator=(IrradianceVolume&&) = delete;

    /// Lay the grid out. Every probe starts invalid and queued. REFUSES a zero count, a spacing
    /// that is not positive, or no rays.
    [[nodiscard]] Status configure(const IrradianceVolumeSettings& settings) noexcept;
    [[nodiscard]] const IrradianceVolumeSettings& settings() const noexcept { return settings_; }

    /// The bake: every probe, staged and committed together. Clears the queue.
    VolumeUpdateReport capture_all(const VolumeCaptureContext& context) noexcept;

    /// Queue the probes whose cells touch `region`. Returns how many were newly queued.
    u32 invalidate(const Aabb& region) noexcept;

    /// One budgeted update under the configured policy.
    VolumeUpdateReport update(const VolumeCaptureContext& context) noexcept;

    // --- Queries ---------------------------------------------------------------------------------

    [[nodiscard]] VolumeSample sample(Vec3 position, Vec3 normal) const noexcept;

    /// `IndirectSource`: the volume's radiance, clamped to its nearest probes outside it.
    [[nodiscard]] Vec3 gather(Vec3 position, Vec3 normal) const noexcept override;

    /// The frame's ambient radiance with this volume attached: `flat` where the volume does not
    /// reach or found no visible probe, the volume where it does, blended over one cell outside.
    [[nodiscard]] Vec3 ambient(Vec3 position, Vec3 normal, Vec3 flat) const noexcept;

    /// One probe's irradiance toward `normal`, as a radiance.
    [[nodiscard]] Vec3 probe_radiance(u32 probe, Vec3 normal) const noexcept;

    [[nodiscard]] u32 probe_count() const noexcept { return static_cast<u32>(probes_.size()); }
    [[nodiscard]] const VolumeProbe& probe(u32 index) const noexcept { return probes_[index]; }
    [[nodiscard]] u32 probe_index(u32 x, u32 y, u32 z) const noexcept;
    [[nodiscard]] u32 queue_depth() const noexcept { return queue_depth_; }
    /// Bumped whenever a probe's stored value changes, so a device copy can tell it is stale.
    [[nodiscard]] u64 generation() const noexcept { return generation_; }

    // --- The device copy -------------------------------------------------------------------------

    [[nodiscard]] VolumeTextureLayout texture_layout() const noexcept;

    /// Write every probe as `kVolumeTexelsPerProbe` RGBA texels: red, green and blue SH L1
    /// coefficients (divided by the layout's scale), then (validity, +x, -x, +y) and
    /// (-y, +z, -z, 0) distances. Probe (x, y, z) starts at texel
    /// `(x * kVolumeTexelsPerProbe, y + count_y * z)`. `out` holds `width * height * 4` floats.
    [[nodiscard]] Status pack_texels(Span<f32> out) const noexcept;

private:
    void capture_probe(VolumeProbe& probe, const VolumeCaptureContext& context,
                       u64& rays) const noexcept;
    [[nodiscard]] f32 visibility(const VolumeProbe& probe, Vec3 position, Vec3 normal,
                                 Vec3 query) const noexcept;
    void commit(u32 index, const VolumeProbe& captured) noexcept;
    [[nodiscard]] u32 capture_queued(const VolumeCaptureContext& context, u32 budget,
                                     u64& rays) noexcept;
    [[nodiscard]] u32 refresh_stalest(const VolumeCaptureContext& context, u32 budget,
                                      u64& rays) noexcept;
    [[nodiscard]] u32 count_invalid() const noexcept;

    IrradianceVolumeSettings settings_{};
    Array<VolumeProbe> probes_;
    Array<Vec3> directions_;
    /// First in, first out. `queue_head_` is the next to capture.
    Array<u32> queue_;
    usize queue_head_ = 0;
    u32 queue_depth_ = 0;
    u32 refresh_cursor_ = 0;
    u64 generation_ = 0;
};

}  // namespace cy::rendering::gi
