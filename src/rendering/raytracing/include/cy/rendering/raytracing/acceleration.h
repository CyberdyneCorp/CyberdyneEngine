#pragma once
// The acceleration structure service: lifecycle, budget, top level, and the query. Task 9.4.
//
// `ray-tracing-infrastructure` — "Ray tracing infrastructure is a renderer service", "Structure
// lifecycle and budget", "Ray query interface", "Capability gating and fallback", "Ray tracing
// diagnostics".
//
// ================================================================================================
// ONE SERVICE, MANY CONSUMERS — WHICH IS A STATEMENT ABOUT OWNERSHIP
// ================================================================================================
//
// Reflections, shadows, ambient occlusion, GI probe tracing and gameplay queries all reach a
// surface through `trace()`. None of them may build, refit or own a structure, and the way that
// rule is enforced here is that there is no API for them to do it with: `declare_geometry` is keyed
// by a geometry identity, not by a consumer, and the top level is rebuilt by `update()` and by
// nothing else. `Consumer` exists only so that ray counts are attributable.
//
// ================================================================================================
// THE BUDGET IS THE PART THAT IS EASY TO GET WRONG
// ================================================================================================
//
// "Many new instances become resident in one frame" is the ordinary case in a streaming world, not
// the pathological one. The service therefore never builds more than its budget in a frame, and an
// instance whose geometry has no structure yet is EXCLUDED FROM TRACING rather than stalling the
// frame — it is missing from the traced world for a frame or two and its absence is counted. A
// service that blocked instead would convert a residency spike into a hitch, which is the defect
// the budget exists to prevent.
//
// The queue is ordered by the best importance and screen coverage among the instances waiting on
// it, so the structure a player is looking at is built before the one behind them.
//
// ================================================================================================
// WHAT `update()` DOES, IN ORDER
// ================================================================================================
//
//   1. evict every geometry that is no longer declared and no longer referenced;
//   2. refit the geometries whose vertices moved, up to the refit budget, rebuilding instead when
//      the refit's bounds growth has passed the adapter's quality floor;
//   3. build pending geometries in priority order, up to the build budget;
//   4. rebuild the top level from the instances whose geometry has a structure.
//
// Step 4 is unconditional, per "The top-level structure SHALL be rebuilt per frame from resident
// instances" — a top level rebuilt only when something moved would silently keep an evicted
// instance traceable.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/bvh.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/raytracing/geometry.h>
#include <cy/rendering/raytracing/query.h>

namespace cy::rendering::rt {

/// The identity a structure is cached under: an asset and the LOD or proxy level chosen from it.
/// Two instances of the same geometry share one structure because they share one of these.
using GeometryId = u64;

inline constexpr u32 kInvalidHandle = ~0U;

/// What the service may spend on structures in one frame.
struct BuildBudget {
    u32 max_builds_per_frame = 4;
    u32 max_refits_per_frame = 32;
    /// The cost half of "the number and cost of builds SHALL be bounded": four builds of a hundred
    /// triangles and four of a million are not the same frame.
    u64 max_build_triangles_per_frame = 250000;
};

/// One instance in the top-level structure.
struct InstanceDescriptor {
    GeometryId geometry = 0;
    /// Object to world.
    Mat4 transform = Mat4::identity();
    /// The GPU scene's identifier for this instance. Reported by every hit unchanged.
    u32 instance_id = kInvalidInstance;
    /// The GPU material table entry, used when the geometry declares no per-triangle materials.
    u32 material_id = 0;
    /// Build priority, both from the ECS's per-object importance and from what the view sees.
    f32 importance = 1.0F;
    f32 screen_coverage = 0.0F;
};

/// What one `update()` did.
struct FrameReport {
    u32 builds = 0;
    u32 rebuilds = 0;
    u32 refits = 0;
    /// Builds that were ready and did not fit in the budget. Deferred, never dropped.
    u32 deferred_builds = 0;
    /// Instances left out of the top level because their geometry has no structure yet.
    u32 excluded_instances = 0;
    u32 traced_instances = 0;
    u64 top_level_rebuild_ns = 0;
};

/// `ray-tracing-infrastructure` — "Ray tracing diagnostics", field for field.
struct Diagnostics {
    /// Structure memory in use, and the same total attributed to the geometry source that asked
    /// for it — which is what "attribute it to geometry sources and assets" needs.
    u64 structure_bytes = 0;
    u64 structure_bytes_by_source[kGeometrySourceCount] = {};
    u32 bottom_level_count = 0;
    u32 instance_count = 0;
    u32 pending_builds = 0;
    FrameReport last_frame{};
    /// Traced rays since construction, per consumer.
    u64 rays[kConsumerCount] = {};
    /// Queries issued while the service was inactive. Not an error — a consumer is allowed to ask
    /// and take the miss — but a number that should be zero once the fallbacks are wired, and the
    /// one that says a consumer is asking without checking.
    u64 queries_while_inactive = 0;
};

/// The renderer service that owns acceleration structures.
///
/// Not thread-safe and not internally threaded. `declare_geometry`, `add_instance` and `update` are
/// the frame thread's; `trace` is re-entrant and const-like once `update` has returned, which is
/// what lets a job system fan a probe gather out over workers between two updates.
class AccelerationService {
public:
    struct ServiceConfig {
        /// What the device reports. `cy::rhi::Capability::RayTracing` today — see query.h on why
        /// that is false on every device this engine can open.
        bool device_supports_ray_tracing = false;
        /// What the renderer profile asked for. False on capable hardware is the case the
        /// specification requires to behave exactly like absent hardware.
        bool enabled_by_profile = true;
        BuildBudget budget{};
    };

    /// A default-constructed service is the one this tree produces: no device support, therefore
    /// `Availability::Unsupported`, therefore every consumer on its fallback.
    AccelerationService() noexcept : AccelerationService(ServiceConfig{}) {}
    explicit AccelerationService(const ServiceConfig& config) noexcept;

    [[nodiscard]] Availability availability() const noexcept { return availability_; }
    [[nodiscard]] bool active() const noexcept { return is_active(availability_); }

    /// A renderer profile turning ray tracing off, or back on, at run time.
    ///
    /// The specification requires the OFF case to behave exactly like a device without the
    /// extension, and this is the switch that exercises it — on capable hardware, where the
    /// difference would otherwise never be observed.
    ///
    /// Two things follow and they are easy to confuse. **What the service REPORTS while off is what
    /// an unsupported device reports**: no structures, no memory, no builds — anything else would
    /// be an observable difference between the two causes, which is the one thing the requirement
    /// forbids. **What the service KEEPS while off is everything**: the structures are not torn
    /// down, so turning it back on costs no rebuild. A profile change is not a residency event, and
    /// a renderer that dropped a scene's structures because a menu was toggled would be one that
    /// hitched on the way back.
    void set_enabled_by_profile(bool enabled) noexcept;

    void set_budget(const BuildBudget& budget) noexcept { budget_ = budget; }
    [[nodiscard]] const BuildBudget& budget() const noexcept { return budget_; }

    // --- Geometry lifecycle -------------------------------------------------------------------

    /// Declare the geometry `id` and queue its structure for an asynchronous build.
    ///
    /// Declaring an id that already exists replaces its build input and queues a rebuild; declaring
    /// one whose structure is already built and unchanged is a no-op, which is what makes this safe
    /// to call every frame from a residency callback.
    ///
    /// While the service is inactive this succeeds and builds nothing: a consumer's residency code
    /// should not have to branch on a capability, and `Diagnostics::structure_bytes` staying zero
    /// is the assertion that nothing was built.
    [[nodiscard]] Status declare_geometry(GeometryId id, const BuildInput& input) noexcept;

    /// Drop the declaration. The structure is evicted at the next `update()` once no instance
    /// references it.
    void release_geometry(GeometryId id) noexcept;

    [[nodiscard]] bool has_structure(GeometryId id) const noexcept;

    /// New posed positions for a `MaintenancePolicy::Refit` geometry, in its own space. Queues a
    /// refit; the count must match the declared positions.
    [[nodiscard]] Status refit_geometry(GeometryId id, Span<const Vec3> positions) noexcept;

    // --- Instances ----------------------------------------------------------------------------

    [[nodiscard]] Expected<u32, Error> add_instance(const InstanceDescriptor& descriptor) noexcept;
    void remove_instance(u32 handle) noexcept;
    [[nodiscard]] Status set_instance_transform(u32 handle, const Mat4& transform) noexcept;

    // --- The frame ----------------------------------------------------------------------------

    FrameReport update() noexcept;

    // --- Queries ------------------------------------------------------------------------------

    /// Trace one ray. A miss and an inactive service are the same answer, deliberately: a consumer
    /// selects its fallback from `availability()` once, not from a hit record per ray.
    [[nodiscard]] RayHit trace(const RayQuery& query) noexcept;

    /// The shadow query, spelled out because it is the one that must not fill in a hit record.
    [[nodiscard]] bool occluded(const Ray& ray, f32 t_max, Consumer consumer) noexcept;

    [[nodiscard]] const Diagnostics& diagnostics() const noexcept { return diagnostics_; }
    void reset_ray_counts() noexcept;

private:
    struct Bottom {
        GeometryId id = 0;
        BuildInput input{};
        Array<Vec3> positions;
        Array<u32> indices;
        Array<u32> triangle_materials;
        Bvh<u32> tree;
        Aabb bounds{};
        /// The bounds the tree was built for. A refit that grows past `refit_quality_floor` of it
        /// is a rebuild instead.
        Aabb built_bounds{};
        u32 instance_refs = 0;
        bool declared = false;
        bool built = false;
        bool needs_build = false;
        bool needs_refit = false;
        bool live = false;
        f32 priority = 0.0F;
        u64 bytes = 0;
    };

    struct Instance {
        InstanceDescriptor descriptor{};
        Mat4 inverse_transform = Mat4::identity();
        u32 bottom = kInvalidHandle;
        bool live = false;
    };

    [[nodiscard]] u32 find_bottom(GeometryId id) const noexcept;
    /// Static because they operate on one `Bottom` and touch no other service state. Keeping them
    /// members is what lets them name the private type.
    [[nodiscard]] static Status store_geometry(Bottom& bottom, const BuildInput& input) noexcept;
    [[nodiscard]] static Status build_bottom(Bottom& bottom) noexcept;
    void evict_unreferenced() noexcept;
    void service_refits(FrameReport& report) noexcept;
    void service_builds(FrameReport& report) noexcept;
    void rebuild_top_level(FrameReport& report) noexcept;
    void account_structure_bytes() noexcept;
    [[nodiscard]] RayHit trace_instance(const Instance& instance, const RayQuery& query,
                                        f32 t_max) const noexcept;

    Availability availability_ = Availability::Unsupported;
    BuildBudget budget_{};
    Array<Bottom> bottoms_;
    Array<Instance> instances_;
    Array<u32> free_instances_;
    /// The top level: a tree over instance world bounds whose payload is the instance slot.
    Bvh<u32> top_;
    Array<Aabb> top_bounds_;
    Array<u32> top_payloads_;
    bool top_dirty_ = true;
    Diagnostics diagnostics_{};
};

}  // namespace cy::rendering::rt
