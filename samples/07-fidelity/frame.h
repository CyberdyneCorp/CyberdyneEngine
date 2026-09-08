#pragma once
// samples/07-fidelity — the frame: what the device draws, and what lights it. Tasks 11.1 and 11.2.
//
// ================================================================================================
// THE FRAME TIME IS A MEASUREMENT AND THE HEADER SAYS EXACTLY WHICH ONE
// ================================================================================================
//
// `render_frames` records the cluster traversal and the visibility pass into one `RenderGraph`,
// submits it, and waits for the device to go idle. The figure it reports per frame is therefore
// SUBMIT TO IDLE on a serialised frame: it contains the dispatches, the barriers the graph derived
// and the submission itself, and it does not contain a presented swapchain because this artefact
// has no window. That is the honest name for it, and the name it is printed under.
//
// `ExecutionResult` carries no timestamps, so nothing finer is available through the graph today.
// A frame timed with the device's own query pool would be the improvement, and it is a change to
// `cy::rendering::GraphExecutor` rather than to this sample.
//
// ================================================================================================
// THE SHADING IS ON THE CPU, OVER THE PIXELS THE DEVICE RESOLVED, AND THAT IS DELIBERATE
// ================================================================================================
//
// `rendering-global-illumination`'s system is a CPU system in this tree: `indirect_diffuse` and
// `indirect_specular` are host functions over the surface and radiance caches, and the ray-query
// capability the hardware tier needs is not among the fourteen the Vulkan backend sets — the GI
// module's own README says so, and it is why the SOFTWARE tier is the default path on this machine.
//
// So the artefact queries the illumination system at points on the scene's own surfaces rather than
// at the pixels the device resolved. That is a smaller claim than "the frame is lit by GI" and it
// is the one this tree supports: the surfaces are the ones the geometry was cooked from, the lights
// are the ones the shot is lit by, and the system is the shipped one. Coupling it to the visibility
// buffer would need a deferred shading pass that has nowhere to run.
//
// The sample is stratified rather than random and its size is a parameter, so the figures it
// reports reproduce.

#include <cy/core/memory/array.h>

#include "scene.h"

namespace cy::sample::fidelity {

struct FrameOptions {
    u32 width = 1280;
    u32 height = 720;
    u32 frames = 48;
    /// Frames rendered before timing starts, and excluded from every statistic.
    ///
    /// WITHOUT THIS THE ARTEFACT MEASURES THE GPU'S POWER STATE. A discrete GPU idles at a low
    /// clock and takes tens of milliseconds of sustained work to reach its boost state, so the
    /// first frames of a cold run are two-thirds slower than the steady state — measured here as
    /// a bimodal median, ~3.7 ms warm against ~5.2 ms cold, on the same binary and the same scene.
    /// That figure is handed to the arbiter as the geometry subsystem's authored cost, so a cold
    /// run and a warm run put the control loop at materially different operating points and the
    /// artefact's verdict flipped with the weather. A frame measured before the device has
    /// clocked up is measuring the device, not the renderer.
    u32 warmup_frames = 12;
    /// `virtual-geometry`'s quality lever: the maximum acceptable geometric error, in pixels.
    f32 threshold_pixels = 1.0F;
    /// Covered pixels shaded through the illumination system, per shot.
    u32 shaded_samples = 512;
};

/// What one run of the device path measured. Every figure here is a count or a duration; nothing
/// is a judgement.
struct FrameReport {
    explicit FrameReport(Allocator& allocator) noexcept : frame_ms(allocator) {}

    /// A real graphics device answered. False means the null backend did, and the caller must
    /// report the act as NOT EVALUATED rather than as satisfied.
    bool device = false;
    const char* backend = "none";
    const char* reason = "";
    u32 validation_errors = 0;

    u32 frames = 0;
    Array<f32> frame_ms;
    f32 median_ms = 0.0F;
    f32 p90_ms = 0.0F;
    f32 worst_ms = 0.0F;

    /// Summed over the run, so a shot that saw nothing cannot hide behind one that saw a lot.
    u64 covered_pixels = 0;
    u64 visible_clusters = 0;
    u32 interior_covered = 0;
    u32 exterior_covered = 0;
    u32 materials_seen = 0;
    bool overflowed = false;
    bool levels_exhausted = false;
};

[[nodiscard]] Status render_frames(const Scene& scene, const FrameOptions& options,
                                   FrameReport& out) noexcept;

/// What the illumination system answered over the surfaces the device resolved.
struct LightReport {
    u32 shaded = 0;
    /// Samples whose indirect diffuse carried any radiance at all.
    u32 with_indirect = 0;
    u32 with_reflection = 0;
    /// Mean luminance of the indirect diffuse and of the indirect specular, over `shaded`.
    f32 mean_indirect = 0.0F;
    f32 mean_reflection = 0.0F;
    /// The largest per-channel spread between the indirect diffuse of any two samples: a scene
    /// whose bounce carried no colour would answer zero here however bright it was.
    f32 indirect_colour_spread = 0.0F;
    /// Which world tier answered the rays, and how many rays each tier took.
    const char* tier = "none";
    u32 software_rays = 0;
    u32 hardware_rays = 0;
    u32 frames_to_converge = 0;
    f32 convergence = 0.0F;
    /// The convergence run reached its target rather than its frame cap.
    bool converged = false;
    f32 gi_ms = 0.0F;
    u32 surfels = 0;
    u32 lights = 0;
};

/// Light the shot: build the illumination system over the scene, converge it, and resolve the
/// indirect diffuse and specular at a stratified sample of points on the scene's own surfaces.
///
/// Needs no device, which is what makes 11.2 evaluable on a machine that has none.
[[nodiscard]] Status light_shot(const Scene& scene, const FrameOptions& options,
                                LightReport& out) noexcept;

}  // namespace cy::sample::fidelity
