#pragma once
// The ray query interface, and the capability that gates it. Task 9.4.
//
// `ray-tracing-infrastructure` — "Ray query interface" and "Capability gating and fallback".
//
// ================================================================================================
// THE THREE-VALUED AVAILABILITY, AND WHY IT IS NOT A BOOL
// ================================================================================================
//
// The specification requires that "a renderer profile disables ray tracing on capable hardware" and
// that "behaviour SHALL match the unsupported case exactly, so the fallback path is exercised and
// testable". A bool cannot express that requirement, because the whole point is that two different
// CAUSES must produce one indistinguishable BEHAVIOUR. `Availability` therefore records the cause
// for the diagnostic and `is_active()` is the only thing any code path is allowed to branch on —
// and `tests/test_query.cpp` asserts that the two inactive causes produce byte-identical
// observable results, which is what stops the fallback from rotting.
//
// ================================================================================================
// WHERE THE RAYS ACTUALLY GO AT M7, SAID PLAINLY
// ================================================================================================
//
// This service executes its queries on the CPU, over the `cy::Bvh` structures it builds in
// `acceleration.cpp`. That is not a stand-in for the interface — the interface is the deliverable,
// and it is what `rendering-global-illumination` consumes as its hardware tier — but it IS a
// stand-in for the device.  Two facts decide it and both are checkable:
//
//   * `cy::rhi` has `Capability::RayTracing` as an enumerator and NOTHING sets it. The Vulkan
//     backend's capability block (src/backends/rhi/vulkan/src/vulkan_instance.cpp) never mentions
//     it, so `device_supports_ray_tracing` is false on every device this engine can open today,
//     including the one in this machine.
//   * layer 4 may not name a Vulkan header (tools/layercheck/layercheck.py, the `gpuapi` rule), so
//     `VK_KHR_acceleration_structure` cannot be reached from here even if the RHI grew the API.
//
// The consequence is stated rather than hidden: on this tree the ray tracing service is INACTIVE by
// default, exactly as it would be on a device without the extension, and every consumer runs its
// software fallback. `ServiceConfig::device_supports_ray_tracing` is what a device backend would
// set, and setting it by hand is how the tests exercise the active path.

#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>

namespace cy::rendering::rt {

/// Why the service is or is not tracing. Only `is_active()` may be branched on.
enum class Availability : u8 {
    /// The device has no ray tracing.
    Unsupported = 0,
    /// The device has it and the renderer profile turned it off. Behaves exactly as `Unsupported`.
    DisabledByProfile,
    Available,
};

[[nodiscard]] constexpr bool is_active(Availability availability) noexcept {
    return availability == Availability::Available;
}

/// The enumerator's own spelling, for a diagnostic. Never null.
[[nodiscard]] const char* availability_name(Availability availability) noexcept;

/// The three query shapes `ray-tracing-infrastructure` requires.
enum class QueryKind : u8 {
    /// The nearest hit in the interval.
    ClosestHit = 0,
    /// Any hit, reported as soon as one is found. Cheaper and unordered.
    AnyHit,
    /// Occlusion only: no hit record is filled in beyond `RayHit::hit`.
    Shadow,
};

/// Who issued a ray. Ray counts are reported per consumer because "traced ray counts per consumer"
/// is a diagnostics requirement and because the GI budget is distributed between these.
enum class Consumer : u8 {
    GlobalIllumination = 0,
    Reflections,
    Shadows,
    AmbientOcclusion,
    Gameplay,
    Count,
};

inline constexpr u32 kConsumerCount = static_cast<u32>(Consumer::Count);

/// The enumerator's own spelling, for a diagnostic. Never null.
[[nodiscard]] const char* consumer_name(Consumer consumer) noexcept;

inline constexpr u32 kInvalidInstance = ~0U;

/// What a query answered with.
///
/// `instance_id` and `material_id` are the GPU scene's and the GPU material table's own
/// identifiers, not indices private to this service: `ray-tracing-infrastructure` requires that "a
/// hit can be shaded through the existing material path", and an identifier that had to be
/// translated first would be a second identity space to keep in step.
struct RayHit {
    bool hit = false;
    f32 t = 0.0F;
    u32 instance_id = kInvalidInstance;
    u32 material_id = 0;
    u32 primitive_index = 0;
    Vec3 position{0.0F, 0.0F, 0.0F};
    /// The geometric normal, in world space, facing the ray.
    Vec3 normal{0.0F, 1.0F, 0.0F};
    f32 bary_u = 0.0F;
    f32 bary_v = 0.0F;
    /// The world-space error the hit geometry declares against its rasterised form — zero for every
    /// source but virtual geometry's proxy. A consumer that must know how wrong a hit may be reads
    /// this rather than assuming exactness.
    f32 declared_error_metres = 0.0F;
};

/// One query.
struct RayQuery {
    Ray ray;
    f32 t_min = 1.0e-3F;
    f32 t_max = 1.0e5F;
    QueryKind kind = QueryKind::ClosestHit;
    Consumer consumer = Consumer::GlobalIllumination;
    /// Skip triangles whose winding faces away from the ray. Off by default: an illumination ray
    /// hitting the back of a wall is how a leak is detected rather than a case to ignore.
    bool cull_back_faces = false;
};

}  // namespace cy::rendering::rt
