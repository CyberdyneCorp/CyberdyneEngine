// The reference physics backend. Task 4.2.1, design.md §4.
//
// See reference/include/cy/servers/physics/reference/server.h for what this simulates and — more
// importantly — what it deliberately does not. The short version: everything the interface has, no
// contact resolution, and bounding volumes instead of shapes.
//
// THE THREE THINGS WORTH KNOWING BEFORE READING
//
// 1. STORAGE IS SLOT ARRAYS WITH GENERATIONS, not `HandlePool`. A pool resolves a handle and that
//    is all this needs from it — but the state hash has to walk every live body in an order that is
//    identical across runs, and a dense slot array walked by index is that order by construction.
//    The generation half of the handle is still what makes a stale handle detectable.
//
// 2. THE BROAD PHASE IS O(n^2). A reference backend's broad phase is the one part where being
//    obviously correct is worth more than being fast, and `body_capacity` bounds it. The Jolt
//    backend is what a real scene runs on.
//
// 3. THE TIMINGS ARE DIAGNOSTICS AND ARE NEVER HASHED. `steady_clock` appears in `step()` for
//    `physics`' "Diagnosing a slow step" scenario and nowhere else; `hash_state()` does not read
//    them, and no simulation value is derived from them. That is the whole of the wall-clock
//    exception `simulation-and-determinism` allows for a diagnostic.

#include <cy/servers/physics/reference/server.h>

#include <cy/core/math/geometry.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/scalar.h>

#include <chrono>
#include <cmath>

namespace cy::physics::reference {
namespace {

/// A shape's data, owned. The description handed to `create_shape` points at the caller's arrays;
/// the cache outlives the call, so the arrays are copied and the stored description is repointed at
/// the copies.
struct ShapeSlot {
    ShapeDescription description;
    Aabb bounds = Aabb::empty();
    u64 key = 0;
    u32 generation = 0;
    u32 references = 0;
    bool live = false;
    Array<Vec3> points;
    Array<Vec3> vertices;
    Array<u32> indices;
    Array<f32> samples;
    Array<CompoundChild> children;

    explicit ShapeSlot(Allocator& allocator) noexcept
        : points(allocator),
          vertices(allocator),
          indices(allocator),
          samples(allocator),
          children(allocator) {}
};

struct MaterialSlot {
    MaterialDescription description;
    u32 generation = 0;
    bool live = false;
};

struct ColliderSlot {
    ShapeHandle shape;
    Transform local;
    MaterialHandle material;
    CollisionFilter filter;
    bool is_trigger = false;
    f32 contact_impulse_threshold = 0.0f;
    bool report_stay = true;
};

struct BodySlot {
    u32 generation = 0;
    bool live = false;
    WorldHandle world;
    Name name;
    MotionType motion = MotionType::Dynamic;
    Transform transform;
    Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    Vec3 angular_velocity{0.0f, 0.0f, 0.0f};
    Vec3 accumulated_force{0.0f, 0.0f, 0.0f};
    Vec3 accumulated_torque{0.0f, 0.0f, 0.0f};
    MassProperties mass;
    f32 inverse_mass = 0.0f;
    f32 linear_damping = 0.05f;
    f32 angular_damping = 0.05f;
    f32 gravity_scale = 1.0f;
    bool allow_sleeping = true;
    bool asleep = false;
    bool teleported = false;
    /// Set by a teleport and consumed by the NEXT step, which is what makes `teleported` true for
    /// exactly the one step a reader of `body_state()` looks at. Clearing it inside the step that
    /// followed the teleport would clear it before anybody saw it.
    bool pending_teleport = false;
    f32 sleep_timer = 0.0f;
    u8 locked_axes = 0;
    UserData user_data = 0;
    Array<ColliderSlot> colliders;

    explicit BodySlot(Allocator& allocator) noexcept : colliders(allocator) {}
};

/// One touching pair, carried between steps.
///
/// KEPT IN AN ARRAY, NOT ONLY IN A MAP, and that is not an optimisation. Exit events are produced
/// by walking the pairs that were touching last step and are not touching now; walking a HashMap to
/// do it would emit them in the map's internal order, and `cy::HashMap`'s hash is seeded per
/// process (see `determinism/hash.h`, which says so of `cy::hash_bytes` in as many words). Two runs
/// would then emit the same exit events in a different order — a divergence that appears only when
/// two pairs separate on the same tick, which is to say, rarely and unreproducibly.
struct PairRecord {
    BodyHandle a;
    BodyHandle b;
    UserData user_data_a = 0;
    UserData user_data_b = 0;
    bool trigger = false;
    bool report_stay = true;
};

struct WorldSlot {
    WorldDescription description;
    u32 generation = 0;
    bool live = false;
    Array<u32> bodies;
    EventBuffer events;
    Array<ConstraintBroken> broken;
    /// The pairs that were touching at the end of the previous step, in detection order, and the
    /// lookup into them. What turns an overlap into enter/stay/exit.
    Array<PairRecord> pairs;
    HashMap<u64, u32> pair_lookup;
    Array<PairRecord> next_pairs;
    HashMap<u64, u32> next_lookup;
    /// Pair keys the caller asked to ignore. A sorted array would be better above a few dozen; a
    /// map is what the ignore list is, and it is the same lookup the touching set uses.
    HashMap<u64, u8> ignored;
    StepStatistics statistics;

    explicit WorldSlot(Allocator& allocator) noexcept
        : bodies(allocator),
          events(allocator),
          broken(allocator),
          pairs(allocator),
          pair_lookup(allocator),
          next_pairs(allocator),
          next_lookup(allocator),
          ignored(allocator) {}
};

/// A collider resolved into world space, which is what every query and the broad phase work on.
struct WorldCollider {
    /// Exact for a plane and a sphere; the transformed local bounds for everything else.
    bool is_plane = false;
    Plane plane;
    bool is_sphere = false;
    Vec3 sphere_center{0.0f, 0.0f, 0.0f};
    f32 sphere_radius = 0.0f;
    Aabb bounds = Aabb::empty();
    CollisionFilter filter;
    MaterialHandle material;
    bool is_trigger = false;
};

/// The axis of an AABB face nearest `point`, as an outward normal. Used wherever a box query has to
/// report a normal: the slab the hit is on is the one whose face the point is closest to.
[[nodiscard]] Vec3 box_normal(const Aabb& box, Vec3 point) noexcept {
    const Vec3 center = box.center();
    const Vec3 half = box.half_extents();
    const Vec3 offset = point - center;
    // Distance to each face, normalised by the half extent so a thin box does not always report its
    // thin axis. A zero half extent is degenerate and reports "far", which keeps it from winning.
    const f32 dx = half.x > 0.0f ? std::fabs(offset.x) / half.x : -1.0f;
    const f32 dy = half.y > 0.0f ? std::fabs(offset.y) / half.y : -1.0f;
    const f32 dz = half.z > 0.0f ? std::fabs(offset.z) / half.z : -1.0f;
    if (dx >= dy && dx >= dz) {
        return Vec3{offset.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
    }
    if (dy >= dz) {
        return Vec3{0.0f, offset.y >= 0.0f ? 1.0f : -1.0f, 0.0f};
    }
    return Vec3{0.0f, 0.0f, offset.z >= 0.0f ? 1.0f : -1.0f};
}

/// The smallest translation that separates two overlapping boxes: the axis, the depth and the
/// point.
struct BoxOverlap {
    bool overlapping = false;
    Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 penetration = 0.0f;
    Vec3 point{0.0f, 0.0f, 0.0f};
};

[[nodiscard]] BoxOverlap overlap_of(const Aabb& a, const Aabb& b) noexcept {
    BoxOverlap out;
    if (!a.intersects(b)) {
        return out;
    }
    const Aabb shared = intersection(a, b);
    const Vec3 extent = shared.size();
    out.overlapping = true;
    out.point = shared.center();
    // The minimum-translation axis. Ties go to X then Y then Z, deterministically, because two runs
    // that picked different axes for the same symmetric overlap would report different normals.
    if (extent.x <= extent.y && extent.x <= extent.z) {
        out.penetration = extent.x;
        out.normal = Vec3{b.center().x >= a.center().x ? 1.0f : -1.0f, 0.0f, 0.0f};
    } else if (extent.y <= extent.z) {
        out.penetration = extent.y;
        out.normal = Vec3{0.0f, b.center().y >= a.center().y ? 1.0f : -1.0f, 0.0f};
    } else {
        out.penetration = extent.z;
        out.normal = Vec3{0.0f, 0.0f, b.center().z >= a.center().z ? 1.0f : -1.0f};
    }
    return out;
}

/// A box's support distance along `normal`: how far its surface reaches from its centre.
[[nodiscard]] f32 box_support(const Aabb& box, Vec3 normal) noexcept {
    const Vec3 half = box.half_extents();
    return (std::fabs(normal.x) * half.x) + (std::fabs(normal.y) * half.y) +
           (std::fabs(normal.z) * half.z);
}

/// How close two surfaces may be before they count as touching rather than overlapping.
///
/// A tenth of a millimetre. Load-bearing rather than cosmetic: see the plane branch of
/// `shape_cast`, where treating an exact touch as a penetration stops a character from walking.
constexpr f32 kTouchTolerance = 1e-4f;

/// One swept-box candidate: how far the mover may travel before it meets this collider.
struct SweepCandidate {
    bool hit = false;
    bool penetrating = false;
    f32 distance = 0.0f;
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

/// Sweep an axis-aligned box against one world-space collider.
///
/// A free function so `shape_cast()` reads as "for every collider, ask this, then keep the best"
/// rather than as two geometry cases nested three levels inside a double loop.
[[nodiscard]] SweepCandidate sweep_candidate(const Aabb& query_bounds, Vec3 direction,
                                             f32 max_distance,
                                             const WorldCollider& target) noexcept {
    SweepCandidate out;
    const Vec3 center = query_bounds.center();
    if (target.is_plane) {
        const f32 support = box_support(query_bounds, target.plane.normal);
        const f32 gap = target.plane.signed_distance(center) - support;
        const f32 approach = dot(target.plane.normal, direction);
        // TOUCHING IS NOT PENETRATING, AND THE DIFFERENCE IS WHY A CHARACTER CAN WALK. A capsule
        // resting exactly on a surface has `gap == 0`; treating that as a penetration reports a hit
        // at distance zero for EVERY direction, including the horizontal one, and the
        // collide-and-slide loop then makes no progress at all — the character stands still on
        // perfectly flat ground. So: inside is `gap` below the tolerance, and a sweep only meets a
        // plane it is moving toward.
        if (gap < -kTouchTolerance) {
            out.hit = true;
            out.penetrating = true;
            out.normal = target.plane.normal;
            return out;
        }
        if (approach < -math::kEpsilon && (math::max(0.0f, gap) / -approach) <= max_distance) {
            out.hit = true;
            out.distance = math::max(0.0f, gap) / -approach;
            out.normal = target.plane.normal;
        }
        return out;
    }
    if (target.bounds.is_empty()) {
        return out;
    }
    // The Minkowski form: sweeping a box against a box is sweeping a POINT against the target grown
    // by the mover's half extents. One ray test rather than a per-axis sweep, and the normal falls
    // out of the same slab test.
    const Vec3 half = query_bounds.half_extents();
    const Aabb expanded = Aabb::from_min_max(target.bounds.min - half, target.bounds.max + half);
    // Shrunk by the touch tolerance for the containment test, for the reason the plane branch above
    // spells out: a capsule resting exactly on a box has its centre exactly on the expanded box's
    // face, and an inclusive test would call that a penetration on every step for ever.
    const Vec3 tolerance{kTouchTolerance, kTouchTolerance, kTouchTolerance};
    const Aabb inside = Aabb::from_min_max(expanded.min + tolerance, expanded.max - tolerance);
    if (!inside.is_empty() && inside.contains(center)) {
        out.hit = true;
        out.penetrating = true;
        // Reported pointing AWAY from the target, so a caller pushes itself out along it rather
        // than deeper in.
        out.normal = -overlap_of(query_bounds, target.bounds).normal;
        return out;
    }
    const Ray ray{center, direction};
    f32 t_min = 0.0f;
    f32 t_max = 0.0f;
    if (!geom::ray_aabb(ray, expanded, max_distance, t_min, t_max)) {
        return out;
    }
    out.hit = true;
    out.distance = t_min > 0.0f ? t_min : 0.0f;
    out.normal = box_normal(expanded, ray.at(out.distance));
    return out;
}

/// The debug colour a body is drawn in. A function rather than a chain of conditional expressions
/// at the call site: five outcomes read as a table here and as a puzzle there.
[[nodiscard]] DebugColor body_color(MotionType motion, bool asleep) noexcept {
    if (motion == MotionType::Static) {
        return DebugColor::Static;
    }
    if (motion == MotionType::Kinematic) {
        return DebugColor::Kinematic;
    }
    return asleep ? DebugColor::DynamicAsleep : DebugColor::DynamicAwake;
}

/// What a pair of world-space colliders decided, once the filter has already accepted them.
struct ColliderContact {
    bool touching = false;
    ContactPoint point;
};

/// Test two world-space colliders against each other.
///
/// A free function so `detect()` reads as "for every pair, ask this" rather than as four levels of
/// nesting with two geometry cases inlined at the bottom of them. Bounding volumes, per the
/// backend's own documented limits — a plane is exact, everything else is its box.
[[nodiscard]] ColliderContact touching_pair(const WorldCollider& a,
                                            const WorldCollider& b) noexcept {
    ColliderContact out;
    if (a.is_plane && b.is_plane) {
        return out;  // two half-spaces always overlap somewhere; that is not a contact
    }
    if (a.is_plane || b.is_plane) {
        const WorldCollider& plane = a.is_plane ? a : b;
        const WorldCollider& solid = a.is_plane ? b : a;
        if (solid.bounds.is_empty()) {
            return out;
        }
        const f32 support = box_support(solid.bounds, plane.plane.normal);
        const f32 distance = plane.plane.signed_distance(solid.bounds.center()) - support;
        if (distance > 0.0f) {
            return out;
        }
        out.touching = true;
        // The normal points from `a` to `b`, so it is the plane's own normal only when `a` IS the
        // plane; the other way round it has to be flipped.
        out.point.normal = a.is_plane ? plane.plane.normal : -plane.plane.normal;
        out.point.penetration = -distance;
        out.point.position = solid.bounds.center() - plane.plane.normal * support;
        return out;
    }
    const BoxOverlap hit = overlap_of(a.bounds, b.bounds);
    if (!hit.overlapping) {
        return out;
    }
    out.touching = true;
    out.point.normal = hit.normal;
    out.point.penetration = hit.penetration;
    out.point.position = hit.point;
    return out;
}

}  // namespace

/// The reference backend.
class ReferenceServer final : public PhysicsServer {
public:
    explicit ReferenceServer(Allocator& allocator) noexcept
        : allocator_(&allocator),
          worlds_(allocator),
          bodies_(allocator),
          shapes_(allocator),
          materials_(allocator),
          shape_cache_(allocator),
          free_worlds_(allocator),
          free_bodies_(allocator),
          free_shapes_(allocator),
          free_materials_(allocator) {}

    ~ReferenceServer() override = default;

    [[nodiscard]] const char* backend_name() const noexcept override { return kBackendName; }

    [[nodiscard]] Status initialize() noexcept override {
        initialized_ = true;
        return ok();
    }

    void shutdown() noexcept override {
        worlds_.clear();
        bodies_.clear();
        shapes_.clear();
        materials_.clear();
        shape_cache_.clear();
        free_worlds_.clear();
        free_bodies_.clear();
        free_shapes_.clear();
        free_materials_.clear();
        initialized_ = false;
    }

    [[nodiscard]] bool is_null_backend() const noexcept override {
        // NOT a null backend. It keeps handle bookkeeping valid, but it also integrates motion,
        // reports events and answers queries — reporting it as null would tell the runtime's report
        // that nothing is simulating, which is the opposite of true.
        return false;
    }

    [[nodiscard]] Capabilities capabilities() const noexcept override {
        Capabilities caps;
        // FALSE, and stated first because it is the one that matters: two solid bodies pass through
        // each other. See the header — a caller is told rather than surprised.
        caps.contact_resolution = false;
        caps.constraints = false;
        caps.triangle_meshes = false;
        caps.convex_hulls = false;
        caps.height_fields = false;
        caps.soft_bodies = false;
        caps.vehicles = false;
        caps.buoyancy = false;
        caps.continuous_collision = false;
        caps.uses_engine_jobs = false;
        // It genuinely is: the integration is a fixed sequence of float operations over a slot
        // array walked by index, with no threading, no allocation on the step path and no clock
        // reading that reaches state. The determinism suite runs over this backend as well as over
        // Jolt.
        caps.determinism = DeterminismPolicy::SamePlatformDeterministic;
        return caps;
    }

    [[nodiscard]] bool stepping() const noexcept override { return stepping_; }

    // --- Worlds --------------------------------------------------------------------------------

    [[nodiscard]] Expected<WorldHandle, Error> create_world(
        const WorldDescription& description) noexcept override {
        if (Status valid = validate(description); !valid) {
            return make_unexpected(valid.error());
        }
        const Expected<u32, Error> slot = acquire_world();
        if (!slot) {
            return make_unexpected(slot.error());
        }
        WorldSlot& world = worlds_[*slot];
        world.description = description;
        world.live = true;
        world.bodies.clear();
        world.broken.clear();
        world.pairs.clear();
        world.pair_lookup.clear();
        world.next_pairs.clear();
        world.next_lookup.clear();
        world.ignored.clear();
        world.statistics = StepStatistics{};
        if (Status reserved = world.events.reserve(description.contact_constraint_capacity);
            !reserved) {
            return make_unexpected(reserved.error());
        }
        return WorldHandle::from_slot(*slot, world.generation);
    }

    [[nodiscard]] Status destroy_world(WorldHandle world) noexcept override {
        WorldSlot* found = resolve(world);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such world");
        }
        // Bodies go with the world. A body that outlived its world would resolve to a slot whose
        // `world` handle is stale, and every operation on it would then have to check two
        // generations instead of one.
        for (const u32 slot : found->bodies) {
            release_body(slot);
        }
        found->bodies.clear();
        found->live = false;
        ++found->generation;
        return free_worlds_.push_back(world.index());
    }

    [[nodiscard]] Status set_gravity(WorldHandle world, Vec3 gravity) noexcept override {
        WorldSlot* found = resolve(world);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such world");
        }
        found->description.gravity = gravity;
        return ok();
    }

    [[nodiscard]] Status step(WorldHandle world, const StepInput& input) noexcept override;

    [[nodiscard]] Expected<StepStatistics, Error> statistics(
        WorldHandle world) const noexcept override {
        const WorldSlot* found = resolve(world);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such world");
        }
        return found->statistics;
    }

    [[nodiscard]] Expected<Span<const ContactEvent>, Error> events(
        WorldHandle world) const noexcept override {
        const WorldSlot* found = resolve(world);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such world");
        }
        return found->events.events();
    }

    [[nodiscard]] Expected<Span<const ConstraintBroken>, Error> broken_constraints(
        WorldHandle world) const noexcept override {
        const WorldSlot* found = resolve(world);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such world");
        }
        return found->broken.span();
    }

    // --- Materials and shapes
    // ---------------------------------------------------------------------

    [[nodiscard]] Expected<MaterialHandle, Error> create_material(
        const MaterialDescription& description) noexcept override {
        const Expected<u32, Error> slot = acquire_material();
        if (!slot) {
            return make_unexpected(slot.error());
        }
        materials_[*slot].description = description;
        materials_[*slot].live = true;
        return MaterialHandle::from_slot(*slot, materials_[*slot].generation);
    }

    [[nodiscard]] Status destroy_material(MaterialHandle material) noexcept override {
        MaterialSlot* found = resolve(material);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such material");
        }
        found->live = false;
        ++found->generation;
        return free_materials_.push_back(material.index());
    }

    [[nodiscard]] Expected<MaterialDescription, Error> material(
        MaterialHandle material) const noexcept override {
        const MaterialSlot* found = resolve(material);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such material");
        }
        return found->description;
    }

    [[nodiscard]] Expected<ShapeHandle, Error> create_shape(
        const ShapeDescription& description) noexcept override;

    [[nodiscard]] Status destroy_shape(ShapeHandle shape) noexcept override {
        ShapeSlot* found = resolve(shape);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such shape");
        }
        // Reference counted, because the cache hands the same handle to every caller that asked for
        // the same shape. Destroying on the first release would invalidate a handle a thousand
        // bodies still hold, which is exactly the sharing the cache exists to provide.
        if (found->references > 1) {
            --found->references;
            return ok();
        }
        (void)shape_cache_.remove(found->key);
        found->live = false;
        found->references = 0;
        ++found->generation;
        return free_shapes_.push_back(shape.index());
    }

    [[nodiscard]] Expected<ShapeStatistics, Error> shape_statistics() const noexcept override {
        return shape_statistics_;
    }

    [[nodiscard]] Status update_height_field(ShapeHandle shape, u32 x, u32 z, u32 width, u32 depth,
                                             Span<const f32> samples) noexcept override;

    // --- Bodies --------------------------------------------------------------------------------

    [[nodiscard]] Expected<BodyHandle, Error> create_body(
        WorldHandle world, const BodyDescription& description) noexcept override;

    [[nodiscard]] Status create_bodies(WorldHandle world, Span<const BodyDescription> descriptions,
                                       Span<BodyHandle> out) noexcept override {
        if (out.size() < descriptions.size()) {
            return fail(ErrorCode::BufferTooSmall,
                        "physics: the output span is smaller than the description span");
        }
        for (usize index = 0; index < descriptions.size(); ++index) {
            const Expected<BodyHandle, Error> body = create_body(world, descriptions[index]);
            if (!body) {
                // Everything created so far is destroyed: a partial bulk registration would leave a
                // terrain cell half-collidable, which is worse than one that failed outright.
                for (usize undo = 0; undo < index; ++undo) {
                    (void)destroy_body(out[undo]);
                }
                return make_unexpected(body.error());
            }
            out[index] = *body;
        }
        return ok();
    }

    [[nodiscard]] Status destroy_body(BodyHandle body) noexcept override {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        if (WorldSlot* world = resolve(found->world); world != nullptr) {
            for (usize index = 0; index < world->bodies.size(); ++index) {
                if (world->bodies[index] == body.index()) {
                    world->bodies.erase(index);
                    break;
                }
            }
        }
        release_body(body.index());
        return ok();
    }

    [[nodiscard]] Status destroy_bodies(Span<const BodyHandle> bodies) noexcept override {
        for (const BodyHandle handle : bodies) {
            if (Status destroyed = destroy_body(handle); !destroyed) {
                return destroyed;
            }
        }
        return ok();
    }

    [[nodiscard]] Expected<BodyState, Error> body_state(BodyHandle body) const noexcept override {
        const BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        BodyState state;
        state.transform = found->transform;
        state.linear_velocity = found->linear_velocity;
        state.angular_velocity = found->angular_velocity;
        state.motion = found->motion;
        state.asleep = found->asleep;
        state.teleported = found->teleported;
        return state;
    }

    [[nodiscard]] Expected<MassProperties, Error> mass_properties(
        BodyHandle body) const noexcept override {
        const BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        return found->mass;
    }

    [[nodiscard]] Expected<UserData, Error> body_user_data(
        BodyHandle body) const noexcept override {
        const BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        return found->user_data;
    }

    [[nodiscard]] bool body_alive(BodyHandle body) const noexcept override {
        return resolve(body) != nullptr;
    }

    [[nodiscard]] Status set_body_transform(BodyHandle body, const Transform& transform,
                                            TeleportMode mode) noexcept override {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        found->transform = transform;
        if (mode == TeleportMode::Teleport) {
            found->pending_teleport = true;
            found->asleep = false;
            found->sleep_timer = 0.0f;
        }
        return ok();
    }

    [[nodiscard]] Status set_body_velocity(BodyHandle body, Vec3 linear,
                                           Vec3 angular) noexcept override {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        found->linear_velocity = apply_linear_locks(linear, found->locked_axes);
        found->angular_velocity = apply_angular_locks(angular, found->locked_axes);
        wake(*found);
        return ok();
    }

    [[nodiscard]] Status set_body_motion_type(BodyHandle body,
                                              MotionType motion) noexcept override {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        found->motion = motion;
        if (motion != MotionType::Dynamic) {
            found->linear_velocity = Vec3{};
            found->angular_velocity = Vec3{};
        }
        return ok();
    }

    [[nodiscard]] Status set_body_filter(BodyHandle body,
                                         CollisionFilter filter) noexcept override {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        for (ColliderSlot& collider : found->colliders) {
            collider.filter = filter;
        }
        return ok();
    }

    [[nodiscard]] Status set_body_gravity_scale(BodyHandle body, f32 scale) noexcept override {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        found->gravity_scale = scale;
        return ok();
    }

    [[nodiscard]] Status set_body_awake(BodyHandle body, bool awake) noexcept override {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        if (found->motion != MotionType::Dynamic) {
            // Not a no-op: a caller waking a static body believes it is dynamic, and the belief is
            // the bug. Reporting it is cheaper than the hour spent wondering why it never moved.
            return fail(ErrorCode::InvalidArgument,
                        "physics: only a dynamic body sleeps, so only a dynamic body wakes");
        }
        found->asleep = !awake;
        found->sleep_timer = 0.0f;
        return ok();
    }

    [[nodiscard]] Status set_pair_ignored(BodyHandle a, BodyHandle b,
                                          bool ignored) noexcept override {
        BodySlot* first = resolve(a);
        BodySlot* second = resolve(b);
        if (first == nullptr || second == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        WorldSlot* world = resolve(first->world);
        if (world == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such world");
        }
        const u64 key = contact_pair_key(a, b);
        if (!ignored) {
            (void)world->ignored.remove(key);
            return ok();
        }
        const Expected<u8*, Error> inserted = world->ignored.insert(key, 1);
        return inserted ? ok() : Status{make_unexpected(inserted.error())};
    }

    [[nodiscard]] Status add_force(BodyHandle body, Vec3 force) noexcept override {
        return accumulate(body, force, Vec3{});
    }

    [[nodiscard]] Status add_torque(BodyHandle body, Vec3 torque) noexcept override {
        return accumulate(body, Vec3{}, torque);
    }

    [[nodiscard]] Status add_impulse(BodyHandle body, Vec3 impulse) noexcept override {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        if (found->motion != MotionType::Dynamic) {
            return ok();
        }
        wake(*found);
        found->linear_velocity = apply_linear_locks(
            found->linear_velocity + impulse * found->inverse_mass, found->locked_axes);
        return ok();
    }

    [[nodiscard]] Status add_impulse_at(BodyHandle body, Vec3 impulse,
                                        Vec3 world_point) noexcept override {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        if (Status applied = add_impulse(body, impulse); !applied) {
            return applied;
        }
        const Vec3 arm = world_point - found->transform.transform_point(found->mass.center_of_mass);
        return add_angular_impulse(body, cross(arm, impulse));
    }

    [[nodiscard]] Status add_angular_impulse(BodyHandle body, Vec3 impulse) noexcept override {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        if (found->motion != MotionType::Dynamic) {
            return ok();
        }
        wake(*found);
        const Vec3 inertia = found->mass.inertia;
        const Vec3 delta{inertia.x > 0.0f ? impulse.x / inertia.x : 0.0f,
                         inertia.y > 0.0f ? impulse.y / inertia.y : 0.0f,
                         inertia.z > 0.0f ? impulse.z / inertia.z : 0.0f};
        found->angular_velocity =
            apply_angular_locks(found->angular_velocity + delta, found->locked_axes);
        return ok();
    }

    // --- Constraints. Not implemented, and it says so
    // ---------------------------------------------

    [[nodiscard]] Expected<ConstraintHandle, Error> create_constraint(
        WorldHandle, const ConstraintDescription& description) noexcept override {
        // `physics` — "Unsupported feature": the capability query reports it and creation "SHALL
        // fail with a clear diagnostic". The diagnostic names the kind AND the backend, because the
        // first question a reader has is "which backend am I on".
        (void)description;
        return fail(ErrorCode::Unsupported,
                    "the reference physics backend implements no constraints; build with "
                    "CY_PHYSICS=ON for the Jolt backend");
    }

    [[nodiscard]] Status destroy_constraint(ConstraintHandle) noexcept override {
        return fail(ErrorCode::Unsupported,
                    "the reference physics backend implements no constraints");
    }

    [[nodiscard]] Status set_constraint_enabled(ConstraintHandle, bool) noexcept override {
        return fail(ErrorCode::Unsupported,
                    "the reference physics backend implements no constraints");
    }

    [[nodiscard]] Status set_constraint_motor(ConstraintHandle,
                                              const MotorSettings&) noexcept override {
        return fail(ErrorCode::Unsupported,
                    "the reference physics backend implements no constraints");
    }

    // --- Queries -------------------------------------------------------------------------------

    [[nodiscard]] Expected<RayCastHit, Error> raycast(
        WorldHandle world, const RayCastInput& input,
        const QueryFilter& filter) const noexcept override;

    [[nodiscard]] Expected<u32, Error> raycast_all(WorldHandle world, const RayCastInput& input,
                                                   const QueryFilter& filter,
                                                   Span<RayCastHit> out) const noexcept override;

    [[nodiscard]] Expected<ShapeCastHit, Error> shape_cast(
        WorldHandle world, const ShapeCastInput& input,
        const QueryFilter& filter) const noexcept override;

    [[nodiscard]] Expected<u32, Error> overlap(WorldHandle world, const OverlapInput& input,
                                               const QueryFilter& filter,
                                               Span<OverlapHit> out) const noexcept override;

    [[nodiscard]] Expected<u32, Error> overlap_point(WorldHandle world, Vec3 point,
                                                     const QueryFilter& filter,
                                                     Span<OverlapHit> out) const noexcept override;

    [[nodiscard]] Expected<ClosestPoint, Error> closest_point(
        WorldHandle world, const ClosestPointInput& input,
        const QueryFilter& filter) const noexcept override;

    // --- Determinism and debugging ------------------------------------------------------------

    [[nodiscard]] DeterminismPolicy determinism_policy() const noexcept override {
        return DeterminismPolicy::SamePlatformDeterministic;
    }

    [[nodiscard]] Status hash_state(WorldHandle world,
                                    determinism::StateHashTree& tree) const noexcept override;

    [[nodiscard]] Status debug_draw(WorldHandle world, DebugDrawFlags flags,
                                    DebugDrawSink& sink) const noexcept override;

private:
    // --- Slot management ------------------------------------------------------------------------

    [[nodiscard]] Expected<u32, Error> acquire_world() noexcept;
    [[nodiscard]] Expected<u32, Error> acquire_material() noexcept;
    [[nodiscard]] Expected<u32, Error> acquire_shape() noexcept;
    [[nodiscard]] Expected<u32, Error> acquire_body() noexcept;
    void release_body(u32 slot) noexcept;

    [[nodiscard]] WorldSlot* resolve(WorldHandle handle) noexcept {
        return const_cast<WorldSlot*>(static_cast<const ReferenceServer*>(this)->resolve(handle));
    }
    [[nodiscard]] const WorldSlot* resolve(WorldHandle handle) const noexcept {
        if (handle.is_null() || handle.index() >= worlds_.size()) {
            return nullptr;
        }
        const WorldSlot& slot = worlds_[handle.index()];
        return (slot.live && slot.generation == handle.generation()) ? &slot : nullptr;
    }
    [[nodiscard]] BodySlot* resolve(BodyHandle handle) noexcept {
        return const_cast<BodySlot*>(static_cast<const ReferenceServer*>(this)->resolve(handle));
    }
    [[nodiscard]] const BodySlot* resolve(BodyHandle handle) const noexcept {
        if (handle.is_null() || handle.index() >= bodies_.size()) {
            return nullptr;
        }
        const BodySlot& slot = bodies_[handle.index()];
        return (slot.live && slot.generation == handle.generation()) ? &slot : nullptr;
    }
    [[nodiscard]] ShapeSlot* resolve(ShapeHandle handle) noexcept {
        return const_cast<ShapeSlot*>(static_cast<const ReferenceServer*>(this)->resolve(handle));
    }
    [[nodiscard]] const ShapeSlot* resolve(ShapeHandle handle) const noexcept {
        if (handle.is_null() || handle.index() >= shapes_.size()) {
            return nullptr;
        }
        const ShapeSlot& slot = shapes_[handle.index()];
        return (slot.live && slot.generation == handle.generation()) ? &slot : nullptr;
    }
    [[nodiscard]] MaterialSlot* resolve(MaterialHandle handle) noexcept {
        return const_cast<MaterialSlot*>(
            static_cast<const ReferenceServer*>(this)->resolve(handle));
    }
    [[nodiscard]] const MaterialSlot* resolve(MaterialHandle handle) const noexcept {
        if (handle.is_null() || handle.index() >= materials_.size()) {
            return nullptr;
        }
        const MaterialSlot& slot = materials_[handle.index()];
        return (slot.live && slot.generation == handle.generation()) ? &slot : nullptr;
    }

    static void wake(BodySlot& body) noexcept {
        body.asleep = false;
        body.sleep_timer = 0.0f;
    }

    [[nodiscard]] Status accumulate(BodyHandle body, Vec3 force, Vec3 torque) noexcept {
        BodySlot* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "physics: no such body");
        }
        if (found->motion != MotionType::Dynamic) {
            return ok();
        }
        wake(*found);
        found->accumulated_force += force;
        found->accumulated_torque += torque;
        return ok();
    }

    /// A collider in world space. `collider` indexes `body.colliders`.
    [[nodiscard]] WorldCollider world_collider(const BodySlot& body, usize collider) const noexcept;

    /// The union of a body's colliders' world bounds. Empty for a body whose only collider is a
    /// plane, which is why the broad phase tests planes separately.
    [[nodiscard]] Aabb world_bounds(const BodySlot& body) const noexcept;

    /// The whole filter decision for a query against one collider.
    /// Static: it reads only its arguments. Said in the signature so a reader does not have to
    /// check, and so a future member access shows up as a compile error rather than as a quiet
    /// dependency on server state from inside a query.
    [[nodiscard]] static bool query_accepts(const WorldSlot& world, const QueryFilter& filter,
                                            const BodySlot& body, BodyHandle handle,
                                            const WorldCollider& collider) noexcept;

    /// `physics`: a query mid-step is rejected in development builds. See server.h.
    [[nodiscard]] Status reject_if_stepping() const noexcept {
        return reject_query_during_step(stepping_);
    }

    /// What one body pair's colliders decided, plus the two event properties that come from the
    /// colliders rather than from the geometry.
    struct PairContact {
        bool touching = false;
        bool trigger = false;
        bool report_stay = true;
        ContactPoint point;
    };

    void integrate(WorldSlot& world, const StepInput& input) noexcept;
    void detect(WorldSlot& world) noexcept;
    /// The first accepted overlapping collider pair between two bodies, or `touching == false`.
    [[nodiscard]] PairContact first_contact(const WorldSlot& world, const BodySlot& a,
                                            const BodySlot& b) const noexcept;
    /// Carry one touching pair into the next step's record and, unless it is a suppressed Stay,
    /// into the event buffer.
    static void record_contact(WorldSlot& world, u64 key, BodyHandle handle_a, BodyHandle handle_b,
                               const BodySlot& a, const BodySlot& b,
                               const PairContact& contact) noexcept;
    /// Emit an Exit for every pair that was touching last step and is not touching now.
    static void emit_exits(WorldSlot& world) noexcept;

    Allocator* allocator_;
    Array<WorldSlot> worlds_;
    Array<BodySlot> bodies_;
    Array<ShapeSlot> shapes_;
    Array<MaterialSlot> materials_;
    /// shape key -> slot index. The whole of `physics`' "Shape sharing".
    HashMap<u64, u32> shape_cache_;
    Array<u32> free_worlds_;
    Array<u32> free_bodies_;
    Array<u32> free_shapes_;
    Array<u32> free_materials_;
    ShapeStatistics shape_statistics_;
    bool stepping_ = false;
    bool initialized_ = false;
};

// ================================================================================================
// SLOT MANAGEMENT
// ================================================================================================
//
// One shape per family: reuse a freed slot when there is one, append otherwise, and bump the
// generation on release so a handle held across the free stops resolving. Written out four times
// rather than templated, because a template over four different slot types with four different
// constructors is longer than the four functions and reads worse.

Expected<u32, Error> ReferenceServer::acquire_world() noexcept {
    if (!free_worlds_.empty()) {
        const u32 slot = free_worlds_[free_worlds_.size() - 1];
        free_worlds_.pop_back();
        return slot;
    }
    const Expected<WorldSlot*, Error> slot = worlds_.emplace_back(*allocator_);
    if (!slot) {
        return make_unexpected(slot.error());
    }
    (*slot)->generation = 1;
    return static_cast<u32>(worlds_.size() - 1);
}

Expected<u32, Error> ReferenceServer::acquire_material() noexcept {
    if (!free_materials_.empty()) {
        const u32 slot = free_materials_[free_materials_.size() - 1];
        free_materials_.pop_back();
        return slot;
    }
    MaterialSlot fresh;
    fresh.generation = 1;
    if (Status pushed = materials_.push_back(fresh); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(materials_.size() - 1);
}

Expected<u32, Error> ReferenceServer::acquire_shape() noexcept {
    if (!free_shapes_.empty()) {
        const u32 slot = free_shapes_[free_shapes_.size() - 1];
        free_shapes_.pop_back();
        return slot;
    }
    const Expected<ShapeSlot*, Error> slot = shapes_.emplace_back(*allocator_);
    if (!slot) {
        return make_unexpected(slot.error());
    }
    (*slot)->generation = 1;
    return static_cast<u32>(shapes_.size() - 1);
}

Expected<u32, Error> ReferenceServer::acquire_body() noexcept {
    if (!free_bodies_.empty()) {
        const u32 slot = free_bodies_[free_bodies_.size() - 1];
        free_bodies_.pop_back();
        return slot;
    }
    const Expected<BodySlot*, Error> slot = bodies_.emplace_back(*allocator_);
    if (!slot) {
        return make_unexpected(slot.error());
    }
    (*slot)->generation = 1;
    return static_cast<u32>(bodies_.size() - 1);
}

void ReferenceServer::release_body(u32 slot) noexcept {
    if (slot >= bodies_.size()) {
        return;
    }
    BodySlot& body = bodies_[slot];
    for (const ColliderSlot& collider : body.colliders) {
        (void)destroy_shape(collider.shape);
    }
    body.colliders.clear();
    body.live = false;
    ++body.generation;
    (void)free_bodies_.push_back(slot);
}

// ================================================================================================
// SHAPES
// ================================================================================================

Expected<ShapeHandle, Error> ReferenceServer::create_shape(
    const ShapeDescription& description) noexcept {
    if (Status valid = validate(description); !valid) {
        return make_unexpected(valid.error());
    }
    ++shape_statistics_.requests;

    // `physics` — "Shape sharing": 1 000 entities with an identical box collider produce ONE shape.
    // The key is `shape_key()`'s, computed in cy_physics rather than here, so that both backends
    // share a shape under exactly the same conditions.
    const u64 key = shape_key(description);
    if (const u32* existing = shape_cache_.find(key); existing != nullptr) {
        ShapeSlot& slot = shapes_[*existing];
        ++slot.references;
        ++shape_statistics_.cache_hits;
        return ShapeHandle::from_slot(*existing, slot.generation);
    }

    const Expected<u32, Error> index = acquire_shape();
    if (!index) {
        return make_unexpected(index.error());
    }
    ShapeSlot& slot = shapes_[*index];
    slot.description = description;
    slot.key = key;
    slot.live = true;
    slot.references = 1;
    slot.points.clear();
    slot.vertices.clear();
    slot.indices.clear();
    slot.samples.clear();
    slot.children.clear();

    // The description handed in points at the CALLER's arrays, which it is free to free the moment
    // this returns. Copying them and repointing the stored description is what makes a shape handle
    // outlive the call that made it.
    if (description.points != nullptr && description.point_count != 0) {
        if (Status added =
                slot.points.append(Span<const Vec3>(description.points, description.point_count));
            !added) {
            return make_unexpected(added.error());
        }
        slot.description.points = slot.points.data();
    }
    if (description.vertices != nullptr && description.vertex_count != 0) {
        if (Status added = slot.vertices.append(
                Span<const Vec3>(description.vertices, description.vertex_count));
            !added) {
            return make_unexpected(added.error());
        }
        slot.description.vertices = slot.vertices.data();
    }
    if (description.indices != nullptr && description.index_count != 0) {
        if (Status added =
                slot.indices.append(Span<const u32>(description.indices, description.index_count));
            !added) {
            return make_unexpected(added.error());
        }
        slot.description.indices = slot.indices.data();
    }
    if (description.height_field.samples != nullptr) {
        const usize count = static_cast<usize>(description.height_field.sample_count_x) *
                            description.height_field.sample_count_z;
        if (Status added =
                slot.samples.append(Span<const f32>(description.height_field.samples, count));
            !added) {
            return make_unexpected(added.error());
        }
        slot.description.height_field.samples = slot.samples.data();
    }
    if (description.children != nullptr && description.child_count != 0) {
        if (Status added = slot.children.append(
                Span<const CompoundChild>(description.children, description.child_count));
            !added) {
            return make_unexpected(added.error());
        }
        slot.description.children = slot.children.data();
    }

    slot.bounds = local_bounds(slot.description);
    const Expected<u32*, Error> cached = shape_cache_.insert(key, *index);
    if (!cached) {
        return make_unexpected(cached.error());
    }
    ++shape_statistics_.unique_shapes;
    return ShapeHandle::from_slot(*index, slot.generation);
}

Status ReferenceServer::update_height_field(ShapeHandle shape, u32 x, u32 z, u32 width, u32 depth,
                                            Span<const f32> samples) noexcept {
    ShapeSlot* found = resolve(shape);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such shape");
    }
    if (found->description.type != ShapeType::HeightField) {
        return fail(ErrorCode::InvalidArgument, "physics: the shape is not a height field");
    }
    const HeightFieldDescription& hf = found->description.height_field;
    if (x + width > hf.sample_count_x || z + depth > hf.sample_count_z) {
        return fail(ErrorCode::OutOfRange,
                    "physics: the height field region falls outside the sample grid");
    }
    if (samples.size() < static_cast<usize>(width) * depth) {
        return fail(ErrorCode::BufferTooSmall, "physics: fewer samples than the region needs");
    }
    // `physics` — "A crater updates collision locally": only the region is written. The rest of the
    // grid is not touched and the shape is not rebuilt, which is the requirement's whole content.
    for (u32 row = 0; row < depth; ++row) {
        for (u32 column = 0; column < width; ++column) {
            found->samples[(static_cast<usize>(z + row) * hf.sample_count_x) + (x + column)] =
                samples[(static_cast<usize>(row) * width) + column];
        }
    }
    found->bounds = local_bounds(found->description);
    // The cache key describes the CONTENTS, so a deformed height field is a different shape and
    // must not answer a later lookup for the original. Re-keyed rather than evicted, so the handle
    // stays valid for every body already carrying it.
    (void)shape_cache_.remove(found->key);
    found->key = shape_key(found->description);
    const Expected<u32*, Error> cached = shape_cache_.insert(found->key, shape.index());
    return cached ? ok() : Status{make_unexpected(cached.error())};
}

// ================================================================================================
// BODIES
// ================================================================================================

Expected<BodyHandle, Error> ReferenceServer::create_body(
    WorldHandle world, const BodyDescription& description) noexcept {
    if (resolve(world) == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    if (Status valid = validate(description); !valid) {
        return make_unexpected(valid.error());
    }

    // `physics` — "Triangle mesh on a dynamic body": rejected "with a diagnostic recommending
    // convex decomposition, since concave dynamic bodies are not supported". Checked before
    // anything is allocated, so the failure costs nothing and names the shape kind.
    for (u32 index = 0; index < description.collider_count; ++index) {
        const ShapeSlot* shape = resolve(description.colliders[index].shape);
        if (shape == nullptr) {
            return fail(ErrorCode::NotFound, "physics: a collider names a shape that is not live");
        }
        if (description.motion != MotionType::Static && is_static_only(shape->description.type)) {
            return fail(ErrorCode::InvalidArgument,
                        "physics: a triangle mesh, height field or plane collider may only carry a "
                        "static body; decompose the mesh into convex hulls for a dynamic one");
        }
    }

    const Expected<u32, Error> index = acquire_body();
    if (!index) {
        return make_unexpected(index.error());
    }
    // Re-resolved AFTER acquire_body(), which may have grown `bodies_` and would have invalidated a
    // pointer taken before it. `worlds_` is a different array, but the habit is the point.
    //
    // CHECKED, NOT ASSERTED, AND THAT IS RULE 2 OF THIS PROJECT PAYING FOR ITSELF. `CY_ASSERT` is
    // compiled out in Profile and Shipping, so an assertion here is not a branch the optimiser can
    // see — and GCC at -O2 rejects the dereference that follows with -Wnull-dereference. The
    // handles were resolved a few lines above, so this can only fire if `acquire_body()` somehow
    // invalidated the world, but the check costs a predictable branch and the alternative was a
    // build that was green in dev and red in two other profiles.
    WorldSlot* world_slot = resolve(world);
    if (world_slot == nullptr) {
        release_body(*index);
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    if (world_slot->bodies.size() >= world_slot->description.body_capacity) {
        release_body(*index);
        return fail(ErrorCode::OutOfRange, "physics: the world's body capacity is full");
    }

    BodySlot& body = bodies_[*index];
    body.live = true;
    body.world = world;
    body.name = description.name;
    body.motion = description.motion;
    body.transform = description.transform;
    body.linear_velocity = apply_linear_locks(description.linear_velocity, description.locked_axes);
    body.angular_velocity =
        apply_angular_locks(description.angular_velocity, description.locked_axes);
    body.accumulated_force = Vec3{};
    body.accumulated_torque = Vec3{};
    body.linear_damping = description.linear_damping;
    body.angular_damping = description.angular_damping;
    body.gravity_scale = description.gravity_scale;
    body.allow_sleeping = description.allow_sleeping;
    body.asleep = description.start_asleep;
    body.teleported = false;
    body.pending_teleport = false;
    body.sleep_timer = 0.0f;
    body.locked_axes = description.locked_axes;
    body.user_data = description.user_data;
    body.colliders.clear();

    // `physics` — "Multiple colliders per body": the compound's mass distribution is "computed from
    // their volumes and the body's density or explicit mass". Volume-weighted, so a heavy base and
    // a light mast put the centre of mass where the base is.
    f32 total_mass = 0.0f;
    Vec3 weighted_center{0.0f, 0.0f, 0.0f};
    Vec3 inertia{0.0f, 0.0f, 0.0f};
    for (u32 collider = 0; collider < description.collider_count; ++collider) {
        const ColliderDescription& source = description.colliders[collider];
        ShapeSlot* shape = resolve(source.shape);
        if (shape == nullptr) {
            release_body(*index);
            return fail(ErrorCode::NotFound, "physics: a collider names a shape that is not live");
        }
        ++shape->references;
        ColliderSlot slot;
        slot.shape = source.shape;
        slot.local = source.local;
        slot.material = source.material;
        slot.filter = source.filter;
        slot.is_trigger = source.is_trigger;
        slot.contact_impulse_threshold = source.contact_impulse_threshold;
        slot.report_stay = source.report_stay;
        if (Status pushed = body.colliders.push_back(slot); !pushed) {
            release_body(*index);
            return make_unexpected(pushed.error());
        }
        if (source.is_trigger) {
            continue;  // a sensor has no mass: it is a volume, not a solid
        }
        f32 density = shape->description.density;
        if (const MaterialSlot* material = resolve(source.material); material != nullptr) {
            density = material->description.density;
        }
        const f32 piece = volume(shape->description) * density;
        total_mass += piece;
        weighted_center += source.local.translation * piece;
        inertia += unit_inertia(shape->description) * piece;
    }

    body.mass.mass = description.mass > 0.0f ? description.mass : total_mass;
    if (description.override_center_of_mass) {
        body.mass.center_of_mass = description.center_of_mass;
    } else if (total_mass > 0.0f) {
        body.mass.center_of_mass = weighted_center / total_mass;
    } else {
        body.mass.center_of_mass = Vec3{};
    }
    // An explicit mass rescales the derived tensor rather than replacing it: the SHAPE of the
    // inertia is the geometry's and only its magnitude is the mass's.
    if (total_mass > 0.0f) {
        inertia = inertia * (body.mass.mass / total_mass);
    }
    body.mass.inertia = inertia;
    body.inverse_mass = (description.motion == MotionType::Dynamic && body.mass.mass > 0.0f)
                            ? 1.0f / body.mass.mass
                            : 0.0f;

    if (Status pushed = world_slot->bodies.push_back(*index); !pushed) {
        release_body(*index);
        return make_unexpected(pushed.error());
    }
    return BodyHandle::from_slot(*index, body.generation);
}

// ================================================================================================
// THE STEP
// ================================================================================================

void ReferenceServer::integrate(WorldSlot& world, const StepInput& input) noexcept {
    const f32 delta = input.delta_seconds;
    const Vec3 gravity = world.description.gravity;
    const Tuning& tuning = world.description.tuning;

    // Insertion order, which is the order `world.bodies` is in and the order `hash_state()` walks.
    // Nothing here depends on the order — the bodies do not interact — but keeping every walk the
    // same order is what makes a future one that DOES interact deterministic by default.
    for (const u32 slot : world.bodies) {
        BodySlot& body = bodies_[slot];
        body.teleported = body.pending_teleport;
        body.pending_teleport = false;

        if (body.motion == MotionType::Static || body.asleep) {
            body.accumulated_force = Vec3{};
            body.accumulated_torque = Vec3{};
            continue;
        }

        if (body.motion == MotionType::Dynamic) {
            const Vec3 acceleration =
                (gravity * body.gravity_scale) + (body.accumulated_force * body.inverse_mass);
            body.linear_velocity += acceleration * delta;
            const Vec3 torque = body.accumulated_torque;
            const Vec3 angular_acceleration{
                body.mass.inertia.x > 0.0f ? torque.x / body.mass.inertia.x : 0.0f,
                body.mass.inertia.y > 0.0f ? torque.y / body.mass.inertia.y : 0.0f,
                body.mass.inertia.z > 0.0f ? torque.z / body.mass.inertia.z : 0.0f};
            body.angular_velocity += angular_acceleration * delta;

            // Implicit damping: v /= (1 + c*dt). Unconditionally stable, unlike v *= (1 - c*dt),
            // which reverses the velocity for any damping above 1/dt — a number an author can
            // plausibly type.
            body.linear_velocity /= 1.0f + (body.linear_damping * delta);
            body.angular_velocity /= 1.0f + (body.angular_damping * delta);
        }

        body.linear_velocity = apply_linear_locks(body.linear_velocity, body.locked_axes);
        body.angular_velocity = apply_angular_locks(body.angular_velocity, body.locked_axes);

        body.transform.translation += body.linear_velocity * delta;
        const f32 spin = length(body.angular_velocity);
        if (spin > math::kSmallLength) {
            const Quat rotation = Quat::from_axis_angle(body.angular_velocity / spin, spin * delta);
            body.transform.rotation = normalize(rotation * body.transform.rotation);
        }

        body.accumulated_force = Vec3{};
        body.accumulated_torque = Vec3{};

        if (body.motion == MotionType::Dynamic && body.allow_sleeping) {
            const bool slow = length(body.linear_velocity) < tuning.sleep_linear_velocity &&
                              length(body.angular_velocity) < tuning.sleep_angular_velocity;
            body.sleep_timer = slow ? body.sleep_timer + delta : 0.0f;
            if (body.sleep_timer >= tuning.time_before_sleep_seconds) {
                body.asleep = true;
                body.linear_velocity = Vec3{};
                body.angular_velocity = Vec3{};
            }
        }
    }
}

ReferenceServer::PairContact ReferenceServer::first_contact(const WorldSlot& world,
                                                            const BodySlot& a,
                                                            const BodySlot& b) const noexcept {
    PairContact out;
    for (usize ai = 0; ai < a.colliders.size(); ++ai) {
        const WorldCollider wa = world_collider(a, ai);
        for (usize bi = 0; bi < b.colliders.size(); ++bi) {
            const WorldCollider wb = world_collider(b, bi);
            if (!pair_collides(world.description.matrix, wa.filter, wb.filter)) {
                continue;
            }
            const ColliderContact contact = touching_pair(wa, wb);
            if (!contact.touching) {
                continue;
            }
            // The FIRST accepted overlapping collider pair decides the body pair. A real solver
            // would merge every manifold; a reference backend reports one contact and says so.
            out.touching = true;
            out.point = contact.point;
            out.trigger = wa.is_trigger || wb.is_trigger;
            out.report_stay = a.colliders[ai].report_stay && b.colliders[bi].report_stay;
            return out;
        }
    }
    return out;
}

void ReferenceServer::emit_exits(WorldSlot& world) noexcept {
    // In the PREVIOUS step's detection order — see PairRecord for why the order is load-bearing.
    for (const PairRecord& record : world.pairs) {
        if (world.next_lookup.contains(contact_pair_key(record.a, record.b))) {
            continue;
        }
        ContactEvent event;
        event.a = record.a;
        event.b = record.b;
        event.user_data_a = record.user_data_a;
        event.user_data_b = record.user_data_b;
        event.phase = ContactPhase::Exit;
        event.trigger = record.trigger;
        event.point_count = 0;
        (void)world.events.push(event);
    }
}

void ReferenceServer::detect(WorldSlot& world) noexcept {
    world.events.clear();
    world.next_pairs.clear();
    world.next_lookup.clear();

    u32 contacts = 0;
    for (usize first = 0; first < world.bodies.size(); ++first) {
        const BodySlot& a = bodies_[world.bodies[first]];
        for (usize second = first + 1; second < world.bodies.size(); ++second) {
            const BodySlot& b = bodies_[world.bodies[second]];
            // Two static bodies never move, so a pair of them can only ever report Stay forever.
            if (a.motion == MotionType::Static && b.motion == MotionType::Static) {
                continue;
            }
            const BodyHandle handle_a = BodyHandle::from_slot(world.bodies[first], a.generation);
            const BodyHandle handle_b = BodyHandle::from_slot(world.bodies[second], b.generation);
            const u64 key = contact_pair_key(handle_a, handle_b);
            if (world.ignored.contains(key)) {
                continue;  // `physics`: per-body ignore lists, for exceptions
            }
            const PairContact contact = first_contact(world, a, b);
            if (!contact.touching) {
                continue;
            }
            ++contacts;
            record_contact(world, key, handle_a, handle_b, a, b, contact);
        }
    }

    emit_exits(world);

    // Swap rather than copy: the next step's "previous" is this step's "next", and a copy would be
    // an allocation per step for no benefit.
    Array<PairRecord> pairs = static_cast<Array<PairRecord>&&>(world.pairs);
    world.pairs = static_cast<Array<PairRecord>&&>(world.next_pairs);
    world.next_pairs = static_cast<Array<PairRecord>&&>(pairs);
    HashMap<u64, u32> lookup = static_cast<HashMap<u64, u32>&&>(world.pair_lookup);
    world.pair_lookup = static_cast<HashMap<u64, u32>&&>(world.next_lookup);
    world.next_lookup = static_cast<HashMap<u64, u32>&&>(lookup);

    world.statistics.contact_count = contacts;
}

void ReferenceServer::record_contact(WorldSlot& world, u64 key, BodyHandle handle_a,
                                     BodyHandle handle_b, const BodySlot& a, const BodySlot& b,
                                     const PairContact& contact) noexcept {
    const bool was_touching = world.pair_lookup.contains(key);
    PairRecord record;
    record.a = handle_a;
    record.b = handle_b;
    record.user_data_a = a.user_data;
    record.user_data_b = b.user_data;
    record.trigger = contact.trigger;
    record.report_stay = contact.report_stay;
    if (Status pushed = world.next_pairs.push_back(record); !pushed) {
        return;
    }
    const Expected<u32*, Error> inserted =
        world.next_lookup.insert(key, static_cast<u32>(world.next_pairs.size() - 1));
    (void)inserted;

    if (was_touching && !contact.report_stay) {
        return;  // `physics` — "Contact filtering": no event flood on a resting contact
    }
    ContactEvent event;
    event.a = handle_a;
    event.b = handle_b;
    event.user_data_a = a.user_data;
    event.user_data_b = b.user_data;
    event.phase = was_touching ? ContactPhase::Stay : ContactPhase::Enter;
    event.trigger = contact.trigger;
    event.point_count = 1;
    event.points[0] = contact.point;
    // Zero, and honestly so: this backend does not resolve contacts, so there is no impulse to
    // report. A caller that scales an impact sound by it hears nothing, which is the correct answer
    // for a backend that did not stop anything.
    event.total_impulse = 0.0f;
    (void)world.events.push(event);
}

Status ReferenceServer::step(WorldHandle world, const StepInput& input) noexcept {
    WorldSlot* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    if (!(input.delta_seconds > 0.0f) || !math::is_finite(input.delta_seconds)) {
        return fail(ErrorCode::InvalidArgument,
                    "physics: the step must be a positive finite number of seconds");
    }
    if (stepping_) {
        return fail(ErrorCode::Unavailable, "physics: the world is already stepping");
    }

    // DIAGNOSTICS ONLY. Never hashed, never read by simulation — see the file header.
    const auto started = std::chrono::steady_clock::now();
    stepping_ = true;
    integrate(*found, input);
    const auto integrated = std::chrono::steady_clock::now();
    detect(*found);
    const auto finished = std::chrono::steady_clock::now();
    stepping_ = false;

    u32 active = 0;
    for (const u32 slot : found->bodies) {
        const BodySlot& body = bodies_[slot];
        if (body.motion != MotionType::Static && !body.asleep) {
            ++active;
        }
    }
    found->statistics.body_count = static_cast<u32>(found->bodies.size());
    found->statistics.active_body_count = active;
    found->statistics.constraint_count = 0;
    // One island per active body: this backend does not couple bodies, so no two of them are ever
    // in the same island. Reported rather than left at zero, because "no islands" and "every body
    // is its own island" are different facts and a profiler reader would misread the first.
    found->statistics.island_count = active;
    found->statistics.tick = input.tick;
    found->statistics.solve_ns = 0;
    found->statistics.broad_phase_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - integrated).count();
    found->statistics.narrow_phase_ns = found->statistics.broad_phase_ns;
    found->statistics.total_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
    return ok();
}

// ================================================================================================
// WORLD-SPACE COLLIDERS
// ================================================================================================

WorldCollider ReferenceServer::world_collider(const BodySlot& body, usize collider) const noexcept {
    WorldCollider out;
    const ColliderSlot& slot = body.colliders[collider];
    out.filter = slot.filter;
    out.material = slot.material;
    out.is_trigger = slot.is_trigger;
    const ShapeSlot* shape = resolve(slot.shape);
    if (shape == nullptr) {
        return out;
    }
    const Transform world = body.transform * slot.local;
    switch (shape->description.type) {
        case ShapeType::Plane: {
            // A half-space carried by a transform. The point on the plane nearest the origin is
            // `-d * n`; transforming that point and the normal gives the plane in world space
            // without inverting anything.
            const Vec3 point = world.transform_point(shape->description.plane.normal *
                                                     -shape->description.plane.d);
            out.is_plane = true;
            out.plane = Plane::from_point_normal(
                point, normalize(world.rotate_vector(shape->description.plane.normal)));
            return out;
        }
        case ShapeType::Sphere: {
            // Exact, because a sphere's bounds and a sphere are the same set only at the corners,
            // and a ray that grazes a corner would otherwise report a hit that is not there.
            const Vec3 scale = world.scale;
            const f32 largest =
                math::max(std::fabs(scale.x), math::max(std::fabs(scale.y), std::fabs(scale.z)));
            out.is_sphere = true;
            out.sphere_center = world.translation;
            out.sphere_radius = shape->description.radius * largest;
            out.bounds = Aabb::from_center_extents(
                out.sphere_center, Vec3{out.sphere_radius, out.sphere_radius, out.sphere_radius});
            return out;
        }
        default:
            out.bounds = transformed(shape->bounds, world.to_matrix());
            return out;
    }
}

Aabb ReferenceServer::world_bounds(const BodySlot& body) const noexcept {
    Aabb box = Aabb::empty();
    for (usize index = 0; index < body.colliders.size(); ++index) {
        const WorldCollider collider = world_collider(body, index);
        if (!collider.bounds.is_empty()) {
            box.grow(collider.bounds);
        }
    }
    return box;
}

bool ReferenceServer::query_accepts(const WorldSlot& world, const QueryFilter& filter,
                                    const BodySlot& body, BodyHandle handle,
                                    const WorldCollider& collider) noexcept {
    if (!filter.includes(body.motion)) {
        return false;
    }
    if (filter.ignores(handle)) {
        return false;  // `physics` — "Raycast excluding self"
    }
    if (collider.is_trigger && !filter.include_triggers) {
        return false;
    }
    // The SAME rule a contact goes through, so a query cannot hit something the two bodies would
    // not have collided with.
    return pair_collides(world.description.matrix, filter.filter, collider.filter);
}

// ================================================================================================
// QUERIES
// ================================================================================================

namespace {

/// One ray test against one world collider. Returns false when there is no hit within
/// `max_distance`.
[[nodiscard]] bool ray_against(const WorldCollider& collider, const Ray& ray, f32 max_distance,
                               f32& distance, Vec3& normal) noexcept {
    if (collider.is_plane) {
        f32 t = 0.0f;
        if (!geom::ray_plane(ray, collider.plane, max_distance, t)) {
            return false;
        }
        distance = t;
        normal = collider.plane.normal;
        return true;
    }
    if (collider.is_sphere) {
        f32 t = 0.0f;
        const Sphere sphere{collider.sphere_center, collider.sphere_radius};
        if (!geom::ray_sphere(ray, sphere, max_distance, t)) {
            return false;
        }
        distance = t;
        normal = normalize(ray.at(t) - collider.sphere_center);
        return true;
    }
    if (collider.bounds.is_empty()) {
        return false;
    }
    f32 t_min = 0.0f;
    f32 t_max = 0.0f;
    if (!geom::ray_aabb(ray, collider.bounds, max_distance, t_min, t_max)) {
        return false;
    }
    distance = t_min > 0.0f ? t_min : 0.0f;
    normal = box_normal(collider.bounds, ray.at(distance));
    return true;
}

}  // namespace

Expected<RayCastHit, Error> ReferenceServer::raycast(WorldHandle world, const RayCastInput& input,
                                                     const QueryFilter& filter) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const WorldSlot* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    const Ray ray{input.origin, normalize(input.direction)};
    RayCastHit best;
    f32 best_distance = input.max_distance;
    bool any = false;
    for (usize index = 0; index < found->bodies.size(); ++index) {
        const BodySlot& body = bodies_[found->bodies[index]];
        const BodyHandle handle = BodyHandle::from_slot(found->bodies[index], body.generation);
        for (usize collider = 0; collider < body.colliders.size(); ++collider) {
            const WorldCollider world_shape = world_collider(body, collider);
            if (!query_accepts(*found, filter, body, handle, world_shape)) {
                continue;
            }
            f32 distance = 0.0f;
            Vec3 normal{0.0f, 1.0f, 0.0f};
            if (!ray_against(world_shape, ray, best_distance, distance, normal)) {
                continue;
            }
            if (any && distance >= best_distance) {
                continue;
            }
            any = true;
            best_distance = distance;
            best.body = handle;
            best.user_data = body.user_data;
            best.position = ray.at(distance);
            best.normal = normal;
            best.distance = distance;
            best.material = world_shape.material;
            best.trigger = world_shape.is_trigger;
        }
    }
    return best;
}

Expected<u32, Error> ReferenceServer::raycast_all(WorldHandle world, const RayCastInput& input,
                                                  const QueryFilter& filter,
                                                  Span<RayCastHit> out) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const WorldSlot* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    const Ray ray{input.origin, normalize(input.direction)};
    u32 count = 0;
    for (usize index = 0; index < found->bodies.size() && count < out.size(); ++index) {
        const BodySlot& body = bodies_[found->bodies[index]];
        const BodyHandle handle = BodyHandle::from_slot(found->bodies[index], body.generation);
        for (usize collider = 0; collider < body.colliders.size() && count < out.size();
             ++collider) {
            const WorldCollider world_shape = world_collider(body, collider);
            if (!query_accepts(*found, filter, body, handle, world_shape)) {
                continue;
            }
            f32 distance = 0.0f;
            Vec3 normal{0.0f, 1.0f, 0.0f};
            if (!ray_against(world_shape, ray, input.max_distance, distance, normal)) {
                continue;
            }
            RayCastHit hit;
            hit.body = handle;
            hit.user_data = body.user_data;
            hit.position = ray.at(distance);
            hit.normal = normal;
            hit.distance = distance;
            hit.material = world_shape.material;
            hit.trigger = world_shape.is_trigger;
            out[count++] = hit;
        }
    }
    // Sorted by distance, because `physics` says "all hits" and an unsorted answer makes every
    // caller sort — and two of them would sort differently. Insertion sort: the count is the number
    // of surfaces a ray crossed, which is single digits in every real query.
    for (u32 i = 1; i < count; ++i) {
        RayCastHit key = out[i];
        u32 j = i;
        while (j > 0 && out[j - 1].distance > key.distance) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = key;
    }
    return count;
}

Expected<ShapeCastHit, Error> ReferenceServer::shape_cast(
    WorldHandle world, const ShapeCastInput& input, const QueryFilter& filter) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const WorldSlot* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    const ShapeSlot* shape = resolve(input.shape);
    if (shape == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such shape");
    }
    const Aabb query_bounds = transformed(shape->bounds, input.start.to_matrix());
    const Vec3 direction = normalize(input.direction);
    const Vec3 center = query_bounds.center();

    ShapeCastHit best;
    f32 best_distance = input.max_distance;
    bool any = false;
    for (const u32 slot : found->bodies) {
        const BodySlot& body = bodies_[slot];
        const BodyHandle handle = BodyHandle::from_slot(slot, body.generation);
        for (usize collider = 0; collider < body.colliders.size(); ++collider) {
            const WorldCollider target = world_collider(body, collider);
            if (!query_accepts(*found, filter, body, handle, target)) {
                continue;
            }
            const SweepCandidate candidate =
                sweep_candidate(query_bounds, direction, best_distance, target);
            if (!candidate.hit) {
                continue;
            }
            // A penetrating hit always wins: there is nothing further to sweep towards, and the
            // caller has to be told to depenetrate before it does anything else. Otherwise the
            // nearest hit wins.
            if (any && !candidate.penetrating &&
                (best.started_penetrating || candidate.distance >= best_distance)) {
                continue;
            }
            any = true;
            best_distance = candidate.penetrating ? 0.0f : candidate.distance;
            best.body = handle;
            best.user_data = body.user_data;
            best.position = center + direction * candidate.distance;
            best.normal = candidate.normal;
            best.distance = candidate.distance;
            best.fraction =
                input.max_distance > 0.0f ? candidate.distance / input.max_distance : 0.0f;
            best.material = target.material;
            best.trigger = target.is_trigger;
            best.started_penetrating = candidate.penetrating;
        }
    }
    return best;
}

Expected<u32, Error> ReferenceServer::overlap(WorldHandle world, const OverlapInput& input,
                                              const QueryFilter& filter,
                                              Span<OverlapHit> out) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const WorldSlot* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    const ShapeSlot* shape = resolve(input.shape);
    if (shape == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such shape");
    }
    const Aabb query_bounds = transformed(shape->bounds, input.transform.to_matrix());
    u32 count = 0;
    for (usize index = 0; index < found->bodies.size() && count < out.size(); ++index) {
        const BodySlot& body = bodies_[found->bodies[index]];
        const BodyHandle handle = BodyHandle::from_slot(found->bodies[index], body.generation);
        for (usize collider = 0; collider < body.colliders.size() && count < out.size();
             ++collider) {
            const WorldCollider target = world_collider(body, collider);
            if (!query_accepts(*found, filter, body, handle, target)) {
                continue;
            }
            const bool hit =
                target.is_plane
                    ? (target.plane.signed_distance(query_bounds.center()) -
                       box_support(query_bounds, target.plane.normal)) <= 0.0f
                    : (!target.bounds.is_empty() && target.bounds.intersects(query_bounds));
            if (!hit) {
                continue;
            }
            OverlapHit record;
            record.body = handle;
            record.user_data = body.user_data;
            record.trigger = target.is_trigger;
            out[count++] = record;
            break;  // one hit per body: a caller asked which bodies overlap, not which colliders
        }
    }
    return count;
}

Expected<u32, Error> ReferenceServer::overlap_point(WorldHandle world, Vec3 point,
                                                    const QueryFilter& filter,
                                                    Span<OverlapHit> out) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const WorldSlot* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    u32 count = 0;
    for (usize index = 0; index < found->bodies.size() && count < out.size(); ++index) {
        const BodySlot& body = bodies_[found->bodies[index]];
        const BodyHandle handle = BodyHandle::from_slot(found->bodies[index], body.generation);
        for (usize collider = 0; collider < body.colliders.size() && count < out.size();
             ++collider) {
            const WorldCollider target = world_collider(body, collider);
            if (!query_accepts(*found, filter, body, handle, target)) {
                continue;
            }
            bool inside = false;
            if (target.is_plane) {
                inside = target.plane.signed_distance(point) <= 0.0f;
            } else if (target.is_sphere) {
                inside = distance_squared(point, target.sphere_center) <=
                         target.sphere_radius * target.sphere_radius;
            } else {
                inside = !target.bounds.is_empty() && target.bounds.contains(point);
            }
            if (!inside) {
                continue;
            }
            OverlapHit record;
            record.body = handle;
            record.user_data = body.user_data;
            record.trigger = target.is_trigger;
            out[count++] = record;
            break;
        }
    }
    return count;
}

Expected<ClosestPoint, Error> ReferenceServer::closest_point(
    WorldHandle world, const ClosestPointInput& input, const QueryFilter& filter) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const WorldSlot* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    ClosestPoint best;
    f32 best_distance = input.max_distance;
    bool any = false;
    for (usize index = 0; index < found->bodies.size(); ++index) {
        const BodySlot& body = bodies_[found->bodies[index]];
        const BodyHandle handle = BodyHandle::from_slot(found->bodies[index], body.generation);
        for (usize collider = 0; collider < body.colliders.size(); ++collider) {
            const WorldCollider target = world_collider(body, collider);
            if (!query_accepts(*found, filter, body, handle, target)) {
                continue;
            }
            Vec3 position{0.0f, 0.0f, 0.0f};
            Vec3 normal{0.0f, 1.0f, 0.0f};
            f32 distance = 0.0f;
            if (target.is_plane) {
                position = target.plane.project(input.point);
                normal = target.plane.normal;
                distance = std::fabs(target.plane.signed_distance(input.point));
            } else if (target.is_sphere) {
                const Vec3 offset = input.point - target.sphere_center;
                const f32 length_of = length(offset);
                normal =
                    length_of > math::kSmallLength ? offset / length_of : Vec3{0.0f, 1.0f, 0.0f};
                position = target.sphere_center + normal * target.sphere_radius;
                distance = math::max(0.0f, length_of - target.sphere_radius);
            } else {
                if (target.bounds.is_empty()) {
                    continue;
                }
                position = target.bounds.closest_point(input.point);
                const Vec3 offset = input.point - position;
                const f32 length_of = length(offset);
                distance = length_of;
                normal = length_of > math::kSmallLength ? offset / length_of
                                                        : box_normal(target.bounds, input.point);
            }
            if (any && distance >= best_distance) {
                continue;
            }
            any = true;
            best_distance = distance;
            best.body = handle;
            best.user_data = body.user_data;
            best.position = position;
            best.normal = normal;
            best.distance = distance;
            best.material = target.material;
        }
    }
    return best;
}

// ================================================================================================
// DETERMINISM AND DEBUG DRAW
// ================================================================================================

Status ReferenceServer::hash_state(WorldHandle world,
                                   determinism::StateHashTree& tree) const noexcept {
    const WorldSlot* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    if (Status opened = tree.begin(determinism::HashLevel::World, world.bits(), "physics");
        !opened) {
        return opened;
    }
    // INSERTION ORDER, which is `world.bodies`'s order and is identical across two runs that made
    // the same calls. It is not sorted by handle: sorting would be a second order to keep in step
    // with the step's own walk, and the comparison matches children by id rather than by position
    // anyway — so what the order has to be is STABLE, not sorted.
    for (const u32 slot : found->bodies) {
        const BodySlot& body = bodies_[slot];
        const BodyHandle handle = BodyHandle::from_slot(slot, body.generation);
        if (Status opened = tree.begin(determinism::HashLevel::Entity, handle.bits(), "body");
            !opened) {
            return opened;
        }
        tree.mix_f32(body.transform.translation.x);
        tree.mix_f32(body.transform.translation.y);
        tree.mix_f32(body.transform.translation.z);
        tree.mix_f32(body.transform.rotation.x);
        tree.mix_f32(body.transform.rotation.y);
        tree.mix_f32(body.transform.rotation.z);
        tree.mix_f32(body.transform.rotation.w);
        tree.mix_f32(body.linear_velocity.x);
        tree.mix_f32(body.linear_velocity.y);
        tree.mix_f32(body.linear_velocity.z);
        tree.mix_f32(body.angular_velocity.x);
        tree.mix_f32(body.angular_velocity.y);
        tree.mix_f32(body.angular_velocity.z);
        tree.mix_u64(static_cast<u64>(body.motion));
        tree.mix_u64(body.asleep ? 1U : 0U);
        // Not hashed, deliberately: the step timings (presentation), the sleep timer (derived from
        // hashed state) and `teleported` (a presentation flag the renderer reads). Hashing a
        // derived value makes a divergence report point at the symptom.
        if (Status closed = tree.end(); !closed) {
            return closed;
        }
    }
    return tree.end();
}

Status ReferenceServer::debug_draw(WorldHandle world, DebugDrawFlags flags,
                                   DebugDrawSink& sink) const noexcept {
    const WorldSlot* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "physics: no such world");
    }
    for (const u32 body_slot : found->bodies) {
        const BodySlot& body = bodies_[body_slot];
        const DebugColor color = body_color(body.motion, body.asleep);
        if (has_flag(flags, DebugDrawFlags::Colliders)) {
            for (const ColliderSlot& slot : body.colliders) {
                const ShapeSlot* shape = resolve(slot.shape);
                if (shape == nullptr) {
                    continue;
                }
                // `physics` — "Colliders do not match visuals": drawn at the SIMULATED transform,
                // which is the body's own, so a mismatch with the visual mesh is what you see.
                const Transform world_transform = body.transform * slot.local;
                const DebugColor draw = slot.is_trigger ? DebugColor::Trigger : color;
                if (shape->description.type == ShapeType::Sphere) {
                    sink.sphere(world_transform.translation, shape->description.radius, draw);
                } else if (shape->description.type == ShapeType::Capsule) {
                    sink.capsule(world_transform, shape->description.radius,
                                 shape->description.half_height, draw);
                } else if (!shape->bounds.is_empty()) {
                    sink.box(shape->bounds, world_transform, draw);
                }
            }
        }
        if (has_flag(flags, DebugDrawFlags::BroadPhaseBounds)) {
            const Aabb bounds = world_bounds(body);
            if (!bounds.is_empty()) {
                sink.box(bounds, Transform::identity(), DebugColor::Bounds);
            }
        }
        if (has_flag(flags, DebugDrawFlags::CentersOfMass)) {
            sink.sphere(body.transform.transform_point(body.mass.center_of_mass), 0.02f, color);
        }
        if (has_flag(flags, DebugDrawFlags::Velocities)) {
            const Vec3 from = body.transform.translation;
            sink.line(from, from + body.linear_velocity, DebugColor::Velocity);
        }
    }
    if (has_flag(flags, DebugDrawFlags::Contacts)) {
        const Span<const ContactEvent> contacts = found->events.events();
        for (const ContactEvent& event : contacts) {
            for (u32 point = 0; point < event.point_count; ++point) {
                sink.contact(event.points[point].position, event.points[point].normal,
                             event.points[point].penetration);
            }
        }
    }
    return ok();
}

// ================================================================================================
// THE FACTORY
// ================================================================================================

Expected<PhysicsServer*, Error> create_server(Allocator& allocator) noexcept {
    void* storage = allocator.allocate(sizeof(ReferenceServer), alignof(ReferenceServer));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "physics: could not allocate the reference backend");
    }
    return construct_at<ReferenceServer>(storage, allocator);
}

void destroy_server(PhysicsServer* server, Allocator& allocator) noexcept {
    if (server == nullptr) {
        return;
    }
    // `static_cast` and not `dynamic_cast`: the engine compiles with -fno-rtti, so `dynamic_cast`
    // does not exist here. The cast is sound because `create_server` above is the only thing that
    // produces one of these pointers and it always produces a `ReferenceServer`.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* concrete = static_cast<ReferenceServer*>(server);
    concrete->~ReferenceServer();
    allocator.deallocate(concrete, sizeof(ReferenceServer), alignof(ReferenceServer));
}

}  // namespace cy::physics::reference
