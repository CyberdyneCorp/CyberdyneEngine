// SPDX-License-Identifier: MIT
#pragma once
// Illumination classes of the things that move. `rendering-global-illumination` — "Illumination
// classification of dynamic objects".
//
// ================================================================================================
// FOUR CLASSES, AND THE CLASS DECIDES WHAT A MOVE COSTS
// ================================================================================================
//
// | Class              | In the GI scene                    | What a move does                    |
// |--------------------|------------------------------------|-------------------------------------|
// | `Static`           | cards, field placement             | invalidates the region at once      |
// | `SlowDynamic`      | cards, field placement             | one invalidation of where it went,  |
// |                    |                                    | at most once per interval           |
// | `FastDynamic`      | proxy spheres, never the caches    | nothing is invalidated, ever        |
// | `TransientIgnored` | nothing                            | nothing                             |
//
// A FastDynamic object still darkens what it stands on and still shows in a reflection: its proxy
// spheres occlude the indirect diffuse a nearby surface resolves (`proxy_occlusion`, closed form)
// and a reflection ray that meets one before the world takes the proxy's colour lit by the radiance
// cache. Neither touches a cache, so a crowd walking through a lit hall costs the hall nothing.
//
// The class is DERIVED from mobility and size (`classify_illumination`) unless the object carries
// an override. Size is the largest edge of the object's world bounds: a bullet is under
// `transient_max_size_metres` and is ignored, a car is over `slow_dynamic_min_size_metres` and
// participates with its invalidation amortised, and a character in between is a proxy.
//
// ================================================================================================
// SKINNED GEOMETRY: A DEFORMED PROXY NEAR, A COARSE ONE FAR
// ================================================================================================
//
// A skinned object's illumination representation comes from the pose the device computed — the
// caller hands its posed spheres to `pose_object` (one per bone it cares about). Which of the two a
// frame uses is chosen per frame by importance (`select_proxy_fidelity`): within
// `deformed_proxy_distance_metres * importance` of the camera the deformed proxy, beyond it the one
// bounding sphere. A distant crowd is therefore one sphere a member whatever its rig.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/scene.h>

namespace cy::rendering::gi {

enum class IlluminationClass : u8 {
    Static = 0,
    SlowDynamic,
    FastDynamic,
    TransientIgnored,
    Count,
};

inline constexpr u32 kIlluminationClassCount = static_cast<u32>(IlluminationClass::Count);

[[nodiscard]] const char* illumination_class_name(IlluminationClass value) noexcept;

/// Whether an object may move at runtime. The other half of the derivation is its size.
enum class ObjectMobility : u8 {
    Static = 0,
    Movable,
};

struct ClassificationSettings {
    /// A movable object whose largest world-bounds edge is under this is `TransientIgnored`.
    f32 transient_max_size_metres = 0.25F;
    /// A movable object whose largest edge is at least this is `SlowDynamic`; between the two it is
    /// `FastDynamic`.
    f32 slow_dynamic_min_size_metres = 2.5F;
    /// A SlowDynamic object's moves are coalesced and flushed at most once per this many frames.
    u32 slow_dynamic_interval_frames = 8;
    /// A skinned object nearer the camera than this times its importance uses its deformed proxy.
    f32 deformed_proxy_distance_metres = 20.0F;
};

/// The derivation: `Static` mobility is `Static`; a movable object is classed by its size.
[[nodiscard]] IlluminationClass classify_illumination(
    ObjectMobility mobility, f32 size_metres, const ClassificationSettings& settings) noexcept;

/// Which representation a skinned object uses this frame.
enum class ProxyFidelity : u8 {
    /// The spheres `pose_object` handed over: the deformed proxy.
    Deformed = 0,
    /// The one bounding sphere.
    Coarse,
};

[[nodiscard]] ProxyFidelity select_proxy_fidelity(f32 camera_distance_metres, f32 importance,
                                                  const ClassificationSettings& settings) noexcept;

/// The cosine-weighted fraction of the hemisphere about `normal` at `position` that `sphere`
/// covers — the ambient occlusion of one sphere, in closed form: `(r/d)^2 cos(theta)` for a sphere
/// wholly above the horizon, smoothly clipped as it sinks below it, and one inside the sphere.
[[nodiscard]] f32 sphere_occlusion(const Sphere& sphere, Vec3 position, Vec3 normal) noexcept;

/// Where a ray first meets `sphere`, or a negative number. `direction` is unit length.
[[nodiscard]] f32 sphere_hit_distance(const Sphere& sphere, Vec3 origin, Vec3 direction) noexcept;

/// One object as the illumination system is told about it.
struct GiObjectDesc {
    u64 id = 0;
    ObjectMobility mobility = ObjectMobility::Static;
    /// Object-space bounds. The size the class is derived from is the largest edge of these bounds
    /// transformed to the world.
    Aabb local_bounds{};
    Mat4 transform = Mat4::identity();
    /// Overrides the derivation when set.
    bool has_class_override = false;
    IlluminationClass class_override = IlluminationClass::Static;
    /// A skinned object takes its proxy from `pose_object` near the camera.
    bool skinned = false;
    /// Scales the deformed-proxy distance. Two is a hero character; zero is always coarse.
    f32 importance = 1.0F;
    /// The proxy's diffuse colour, which a reflection of it shows.
    Vec3 proxy_albedo{0.5F, 0.5F, 0.5F};
    /// The object's own distance field, for the participating classes. Optional: an object with
    /// none still contributes its cards.
    const AssetDistanceField* field = nullptr;
    /// Object-space cards, for the participating classes. Placed by `transform`: position and
    /// normal, and — under a scale — area, so an emitter's power is the world's.
    Span<const Surfel> surfels;
};

/// A tag on the GI-scene cell and field placement id an object's cards and field live under, so
/// an object never collides with a streamed world cell and an invalidation names it.
inline constexpr u64 kObjectCellTag = 1ULL << 63U;
[[nodiscard]] constexpr u64 object_cell_id(u64 object) noexcept {
    return object | kObjectCellTag;
}

/// One registered object.
struct GiObject {
    explicit GiObject(Allocator& allocator = current_allocator()) noexcept
        : surfels(allocator), posed(allocator), parked_distances(allocator) {}

    u64 id = 0;
    IlluminationClass klass = IlluminationClass::Static;
    ObjectMobility mobility = ObjectMobility::Static;
    Aabb local_bounds{};
    Mat4 transform = Mat4::identity();
    /// The transform the GI scene and the field hold now. Differs from `transform` while a
    /// SlowDynamic object's move is pending.
    Mat4 committed_transform = Mat4::identity();
    bool skinned = false;
    f32 importance = 1.0F;
    Vec3 proxy_albedo{0.5F, 0.5F, 0.5F};
    bool has_field = false;
    /// Object-space cards, copied.
    Array<Surfel> surfels;
    /// The deformed proxy's world-space spheres, as last posed.
    Array<Sphere> posed;
    /// SlowDynamic: a move is waiting, and the union of everywhere the object has been since the
    /// last flush. The flush invalidates that region once.
    bool pending = false;
    Aabb pending_region{};
    u64 last_flush_frame = 0;
    /// What the last `update()` chose for a skinned object.
    ProxyFidelity fidelity = ProxyFidelity::Coarse;
    /// The class an author or a game set, or none: the derivation decides
    /// (`IlluminationSystem::set_object_class`).
    bool has_class_override = false;
    IlluminationClass class_override = IlluminationClass::Static;
    /// The object's own field while it is in no field: a FastDynamic or TransientIgnored object
    /// keeps it here so a class that participates can place it again. Empty with no field.
    u32 parked_dimension = 0;
    Aabb parked_bounds{};
    Array<f32> parked_distances;
};

struct ObjectsFrameReport {
    u32 objects[kIlluminationClassCount] = {};
    /// SlowDynamic moves coalesced into a flush this frame, and the flushes issued.
    u32 moves_coalesced = 0;
    u32 flushes = 0;
    /// FastDynamic proxy spheres in use this frame, and skinned objects by the fidelity chosen.
    u32 proxy_spheres = 0;
    u32 deformed_proxies = 0;
    u32 coarse_proxies = 0;
};

/// The world bounds `transform` gives `local`, and the largest edge of them.
[[nodiscard]] f32 object_size_metres(const Aabb& local, const Mat4& transform) noexcept;

}  // namespace cy::rendering::gi
