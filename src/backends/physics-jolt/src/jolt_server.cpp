// The Jolt physics backend. Task 4.2.2.
//
// See include/cy/backends/physics/jolt/server.h for what crosses the boundary (two functions and no
// JPH type) and for the job-system contract. What follows is the mapping, and the four places where
// Jolt's model and the engine's do not line up exactly are each marked with why.
//
// THE FOUR MISMATCHES, LISTED HERE SO A READER MEETS THEM BEFORE THEY MEET THE CODE:
//
//   1. FILTERING IS PER BODY, NOT PER COLLIDER. The engine puts a layer and a mask on every
//      collider; Jolt puts an object layer on a body and asks a pair filter about two layers. The
//      matrix half maps exactly (ObjectPairFilter). The MASK half cannot — a mask is a property of
//      the body, not of its layer — so it is applied in `OnContactValidate`, where both bodies are
//      in hand, using the body's FIRST collider's filter. A body whose colliders carry different
//      masks is filtered by the first one here and per collider in the reference backend. Reported.
//
//   2. A SENSOR IS A BODY, NOT A SHAPE. Jolt's `mIsSensor` is per body. A body whose colliders are
//      all triggers becomes a sensor; a MIXED body is rejected with a diagnostic rather than
//      silently becoming solid, because "some of this is a trigger" has no representation here.
//
//   3. CONTACT IMPULSES ARE NOT EXPOSED. Jolt's `ContactListener` runs before the solver and the
//      applied impulses are internal to its constraint manager. What the events carry is an
//      ESTIMATE — the relative normal velocity times the reduced mass at contact time — which is
//      the quantity an impact sound wants and is what `contact_impulse_threshold` is compared
//      against. Said in the event's own comment as well, because a number labelled "impulse" that
//      is really an estimate is worse than no number.
//
//   4. CONTACT CALLBACKS ARRIVE ON WORKER THREADS. They are collected under a lock and the event
//      buffer is SORTED at the end of the step, by body pair and phase. Without the sort, two runs
//      of the same scene would produce the same events in a different order — which is a divergence
//      that appears only under load, and only for whoever is reading the events.

#include <cy/backends/physics/jolt/server.h>

#include "jolt_jobs.h"
#include "jolt_shapes.h"

// clang-format off
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/RegisterTypes.h>
// clang-format on

#include <cy/core/base/assert.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/math/scalar.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace cy::physics::jolt {
namespace {

// --- Jolt's global state -------------------------------------------------------------------------
//
// `RegisterDefaultAllocator`, the type factory and `RegisterTypes` are process-wide and must happen
// exactly once. Reference counted so that two servers in one process are legal and the second does
// not reinitialise the first's — which would invalidate every shape the first had built.

std::mutex g_global_mutex;
u32 g_global_users = 0;

void global_acquire() noexcept {
    const std::lock_guard<std::mutex> guard(g_global_mutex);
    if (g_global_users++ != 0) {
        return;
    }
    JPH::RegisterDefaultAllocator();
    // A function-local static rather than `new`: `core-memory-and-containers` says global
    // new/delete are not used in engine code, and this is the one object Jolt's samples allocate
    // that way. Constructed after `RegisterDefaultAllocator()`, which its own containers need.
    static JPH::Factory factory;
    JPH::Factory::sInstance = &factory;
    JPH::RegisterTypes();
}

void global_release() noexcept {
    const std::lock_guard<std::mutex> guard(g_global_mutex);
    CY_ASSERT(g_global_users != 0);
    if (--g_global_users != 0) {
        return;
    }
    JPH::UnregisterTypes();
    JPH::Factory::sInstance = nullptr;
}

/// Jolt's temp allocator, over the engine's.
///
/// `physics` — "Jolt's temp allocator is bridged to an engine arena". One block from the engine
/// allocator, handed out as a stack — which is what Jolt's own `TempAllocatorImpl` does, except
/// that the block is accounted to the engine's simulation domain and shows up in the memory report
/// rather than as an unattributed malloc.
class EngineTempAllocator final : public JPH::TempAllocator {
public:
    EngineTempAllocator(Allocator& allocator, usize size) noexcept
        : allocator_(&allocator), size_(size) {
        base_ = static_cast<u8*>(allocator_->allocate(size_, kAlignment));
    }

    ~EngineTempAllocator() override {
        if (base_ != nullptr) {
            allocator_->deallocate(base_, size_, kAlignment);
        }
    }

    EngineTempAllocator(const EngineTempAllocator&) = delete;
    EngineTempAllocator& operator=(const EngineTempAllocator&) = delete;

    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }

    void* Allocate(JPH::uint size) override {
        if (size == 0) {
            return nullptr;
        }
        const usize aligned = (size + (kAlignment - 1)) & ~(kAlignment - 1);
        if (base_ == nullptr || top_ + aligned > size_) {
            // Jolt's own implementation asserts. Returning null lets the step degrade rather than
            // abort a Shipping build; the arena is sized from the world's capacities below.
            return nullptr;
        }
        void* result = base_ + top_;
        top_ += aligned;
        return result;
    }

    void Free(void* address, JPH::uint size) override {
        if (address == nullptr) {
            return;
        }
        // A stack: Jolt guarantees frees happen in the reverse order of allocation.
        const usize aligned = (size + (kAlignment - 1)) & ~(kAlignment - 1);
        top_ -= aligned;
    }

private:
    static constexpr usize kAlignment = 16;

    Allocator* allocator_;
    u8* base_ = nullptr;
    usize size_ = 0;
    usize top_ = 0;
};

/// The debug colour a body is drawn in. A function rather than a chain of conditional expressions
/// at the call site: five outcomes read as a table here and as a puzzle there.
[[nodiscard]] DebugColor body_color(MotionType motion, bool sensor, bool asleep) noexcept {
    if (sensor) {
        return DebugColor::Trigger;
    }
    if (motion == MotionType::Static) {
        return DebugColor::Static;
    }
    if (motion == MotionType::Kinematic) {
        return DebugColor::Kinematic;
    }
    return asleep ? DebugColor::DynamicAsleep : DebugColor::DynamicAwake;
}

/// One contact as a worker thread reported it, before the deterministic sort.
struct PendingContact {
    u64 a = 0;  ///< engine `BodyHandle::bits()`
    u64 b = 0;
    ContactPhase phase = ContactPhase::Enter;
    bool trigger = false;
    bool report_stay = true;
    u64 user_data_a = 0;
    u64 user_data_b = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 penetration = 0.0f;
    f32 impulse = 0.0f;
};

/// The engine-side record of one body. Jolt owns the simulation state; this owns the identity.
struct JoltBody {
    u32 generation = 0;
    bool live = false;
    u32 world = 0;
    JPH::BodyID id;
    Name name;
    MotionType motion = MotionType::Dynamic;
    CollisionFilter filter;
    MaterialHandle material;
    UserData user_data = 0;
    MassProperties mass;
    u8 locked_axes = 0;
    bool sensor = false;
    bool report_stay = true;
    bool teleported = false;
    bool pending_teleport = false;
    f32 contact_impulse_threshold = 0.0f;
    /// Held so that `destroy_body` releases the cache's references. A body keeps its shapes alive.
    Array<ShapeHandle> shapes;

    explicit JoltBody(Allocator& allocator) noexcept : shapes(allocator) {}
};

}  // namespace

// --- The world
// ------------------------------------------------------------------------------------

class JoltServer;

/// Everything one Jolt world owns.
///
/// HEAP ALLOCATED AND NEVER RELOCATED. `JPH::PhysicsSystem` is neither copyable nor movable, so an
/// `Array<JoltWorld>` cannot exist — and the contact listener holds a pointer to this object across
/// a step, which a relocating container would invalidate mid-solve.
struct JoltWorld final : public JPH::ContactListener {
    JoltWorld(Allocator& allocator, cy::jobs::JobSystem* jobs,
              const WorldDescription& world_description) noexcept
        : description(world_description),
          temp(allocator, temp_arena_bytes(world_description)),
          job_system(jobs, kMaxJoltJobs, kMaxJoltBarriers),
          bodies(allocator),
          events(allocator),
          broken(allocator),
          ignored(allocator),
          pending(allocator),
          previous(allocator) {}

    JoltWorld(const JoltWorld&) = delete;
    JoltWorld& operator=(const JoltWorld&) = delete;

    /// Jolt's own defaults. A step never approaches either, and both are fixed so that a step
    /// allocates nothing.
    static constexpr JPH::uint kMaxJoltJobs = 2048;
    static constexpr JPH::uint kMaxJoltBarriers = 8;

    /// The temp arena, sized from the world's own capacities rather than from a constant, so a
    /// small test world does not reserve the ten megabytes a full scene needs. The multipliers are
    /// Jolt's rough per-object costs with generous headroom; the floor is what a world with no
    /// bodies still needs for the broad-phase update.
    [[nodiscard]] static usize temp_arena_bytes(const WorldDescription& description) noexcept {
        const usize floor = 1U << 20U;  // 1 MiB
        const usize sized = (static_cast<usize>(description.body_capacity) * 512U) +
                            (static_cast<usize>(description.body_pair_capacity) * 128U) +
                            (static_cast<usize>(description.contact_constraint_capacity) * 256U);
        return sized > floor ? sized : floor;
    }

    // --- JPH::ContactListener. CALLED FROM WORKER THREADS ------------------------------------
    //
    // Everything these three write goes through `pending_mutex`, and the ORDER they arrive in is
    // not reproducible — it is whichever worker got there first. The step turns `pending` into the
    // event buffer with a sort, which is where the order becomes deterministic again.

    JPH::ValidateResult OnContactValidate(const JPH::Body& body_a, const JPH::Body& body_b,
                                          JPH::RVec3Arg base_offset,
                                          const JPH::CollideShapeResult& result) override;
    void OnContactAdded(const JPH::Body& body_a, const JPH::Body& body_b,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override;
    void OnContactPersisted(const JPH::Body& body_a, const JPH::Body& body_b,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override;
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override;

    void record(const JPH::Body& body_a, const JPH::Body& body_b,
                const JPH::ContactManifold& manifold, ContactPhase phase) noexcept;

    WorldDescription description;
    BroadPhaseLayers broad_phase_layers;
    ObjectVsBroadPhaseFilter object_vs_broad_phase;
    ObjectPairFilter pair_filter;
    EngineTempAllocator temp;
    EngineJobSystem job_system;
    JPH::PhysicsSystem system;

    /// Engine body slot indices, in creation order. The state hash walks this, so it is the order
    /// two runs must agree on — `JPH::PhysicsSystem`'s own body order is not exposed and would not
    /// be the same thing.
    Array<u32> bodies;
    EventBuffer events;
    Array<ConstraintBroken> broken;
    HashMap<u64, u8> ignored;

    std::mutex pending_mutex;
    Array<PendingContact> pending;
    /// The previous step's pending list, so `OnContactRemoved` can name bodies that Jolt has since
    /// forgotten the properties of.
    Array<PendingContact> previous;

    StepStatistics statistics;
    JoltServer* server = nullptr;
    u32 generation = 0;
    bool live = false;
};

// --- The server
// -----------------------------------------------------------------------------------

class JoltServer final : public PhysicsServer {
public:
    JoltServer(Allocator& allocator, cy::jobs::JobSystem* jobs) noexcept
        : allocator_(&allocator),
          jobs_(jobs),
          worlds_(allocator),
          bodies_(allocator),
          shapes_(allocator),
          materials_(allocator),
          shape_cache_(allocator),
          free_bodies_(allocator),
          free_shapes_(allocator),
          free_materials_(allocator) {}

    ~JoltServer() override { shutdown(); }

    [[nodiscard]] const char* backend_name() const noexcept override { return kBackendName; }

    [[nodiscard]] Status initialize() noexcept override {
        if (initialized_) {
            return ok();
        }
        global_acquire();
        initialized_ = true;
        return ok();
    }

    void shutdown() noexcept override {
        if (!initialized_) {
            return;
        }
        for (usize index = 0; index < worlds_.size(); ++index) {
            destroy_world_slot(static_cast<u32>(index));
        }
        worlds_.clear();
        bodies_.clear();
        shapes_.clear();
        materials_.clear();
        shape_cache_.clear();
        free_bodies_.clear();
        free_shapes_.clear();
        free_materials_.clear();
        initialized_ = false;
        global_release();
    }

    [[nodiscard]] bool is_null_backend() const noexcept override { return false; }

    [[nodiscard]] Capabilities capabilities() const noexcept override {
        Capabilities caps;
        caps.contact_resolution = true;
        caps.constraints = false;  // task 4.2.2 delivers bodies, shapes, events and queries
        caps.triangle_meshes = true;
        caps.convex_hulls = true;
        caps.height_fields = true;
        caps.soft_bodies = false;
        caps.vehicles = false;
        caps.buoyancy = false;
        caps.continuous_collision = true;
        // TRUE ONLY WHEN IT IS TRUE. A running engine job system was given and Jolt's work reaches
        // it; otherwise the work runs on the calling thread and this says so, which is the whole
        // point of reporting it rather than asserting it.
        caps.uses_engine_jobs = jobs_ != nullptr && jobs_->is_running();
        // Jolt is built with CROSS_PLATFORM_DETERMINISTIC (cmake/dependencies.cmake), which fixes
        // the floating-point mode. The engine claims the same-platform half only — see
        // cy/servers/physics/determinism.h, which says why the other half is M9's.
        caps.determinism = DeterminismPolicy::SamePlatformDeterministic;
        return caps;
    }

    [[nodiscard]] bool stepping() const noexcept override { return stepping_; }

    // --- Worlds ---------------------------------------------------------------------------------

    [[nodiscard]] Expected<WorldHandle, Error> create_world(
        const WorldDescription& description) noexcept override {
        if (!initialized_) {
            return fail(ErrorCode::Unavailable, "jolt: initialize() has not been called");
        }
        if (Status valid = validate(description); !valid) {
            return make_unexpected(valid.error());
        }
        void* storage = allocator_->allocate(sizeof(JoltWorld), alignof(JoltWorld));
        if (storage == nullptr) {
            return fail(ErrorCode::OutOfMemory, "jolt: could not allocate a world");
        }
        auto* world = construct_at<JoltWorld>(storage, *allocator_, jobs_, description);
        if (!world->temp.valid()) {
            world->~JoltWorld();
            allocator_->deallocate(storage, sizeof(JoltWorld), alignof(JoltWorld));
            return fail(ErrorCode::OutOfMemory, "jolt: could not allocate the temporary arena");
        }
        world->server = this;
        world->pair_filter.set_matrix(description.matrix);
        world->system.Init(description.body_capacity, kBodyMutexes, description.body_pair_capacity,
                           description.contact_constraint_capacity, world->broad_phase_layers,
                           world->object_vs_broad_phase, world->pair_filter);
        world->system.SetGravity(to_jolt(description.gravity));
        world->system.SetContactListener(world);
        apply_tuning(*world, description.tuning);
        if (Status reserved = world->events.reserve(description.contact_constraint_capacity);
            !reserved) {
            world->~JoltWorld();
            allocator_->deallocate(storage, sizeof(JoltWorld), alignof(JoltWorld));
            return make_unexpected(reserved.error());
        }
        world->generation = 1;
        world->live = true;

        // Worlds are never reused: the slot array only grows. A world is a heavyweight object a
        // session creates a handful of, and a free list here would buy nothing but a way for a
        // stale handle to resolve to a different world with the same generation.
        if (Status pushed = worlds_.push_back(world); !pushed) {
            world->~JoltWorld();
            allocator_->deallocate(storage, sizeof(JoltWorld), alignof(JoltWorld));
            return make_unexpected(pushed.error());
        }
        return WorldHandle::from_slot(static_cast<u32>(worlds_.size() - 1), world->generation);
    }

    [[nodiscard]] Status destroy_world(WorldHandle world) noexcept override {
        if (resolve(world) == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such world");
        }
        destroy_world_slot(world.index());
        return ok();
    }

    [[nodiscard]] Status set_gravity(WorldHandle world, Vec3 gravity) noexcept override {
        JoltWorld* found = resolve(world);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such world");
        }
        found->description.gravity = gravity;
        found->system.SetGravity(to_jolt(gravity));
        return ok();
    }

    [[nodiscard]] Status step(WorldHandle world, const StepInput& input) noexcept override;

    [[nodiscard]] Expected<StepStatistics, Error> statistics(
        WorldHandle world) const noexcept override {
        const JoltWorld* found = resolve(world);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such world");
        }
        return found->statistics;
    }

    [[nodiscard]] Expected<Span<const ContactEvent>, Error> events(
        WorldHandle world) const noexcept override {
        const JoltWorld* found = resolve(world);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such world");
        }
        return found->events.events();
    }

    [[nodiscard]] Expected<Span<const ConstraintBroken>, Error> broken_constraints(
        WorldHandle world) const noexcept override {
        const JoltWorld* found = resolve(world);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such world");
        }
        return found->broken.span();
    }

    // --- Materials and shapes
    // ---------------------------------------------------------------------

    [[nodiscard]] Expected<MaterialHandle, Error> create_material(
        const MaterialDescription& description) noexcept override {
        u32 slot = 0;
        if (!free_materials_.empty()) {
            slot = free_materials_[free_materials_.size() - 1];
            free_materials_.pop_back();
        } else {
            MaterialSlot fresh;
            fresh.generation = 1;
            if (Status pushed = materials_.push_back(fresh); !pushed) {
                return make_unexpected(pushed.error());
            }
            slot = static_cast<u32>(materials_.size() - 1);
        }
        materials_[slot].description = description;
        materials_[slot].live = true;
        return MaterialHandle::from_slot(slot, materials_[slot].generation);
    }

    [[nodiscard]] Status destroy_material(MaterialHandle material) noexcept override {
        MaterialSlot* found = resolve(material);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such material");
        }
        found->live = false;
        ++found->generation;
        return free_materials_.push_back(material.index());
    }

    [[nodiscard]] Expected<MaterialDescription, Error> material(
        MaterialHandle material) const noexcept override {
        const MaterialSlot* found = resolve(material);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such material");
        }
        return found->description;
    }

    [[nodiscard]] Expected<ShapeHandle, Error> create_shape(
        const ShapeDescription& description) noexcept override;
    [[nodiscard]] Status destroy_shape(ShapeHandle shape) noexcept override;

    [[nodiscard]] Expected<ShapeStatistics, Error> shape_statistics() const noexcept override {
        return shape_statistics_;
    }

    [[nodiscard]] Status update_height_field(ShapeHandle shape, u32 x, u32 z, u32 width, u32 depth,
                                             Span<const f32> samples) noexcept override;

    // --- Bodies ---------------------------------------------------------------------------------

    [[nodiscard]] Expected<BodyHandle, Error> create_body(
        WorldHandle world, const BodyDescription& description) noexcept override;

    [[nodiscard]] Status create_bodies(WorldHandle world, Span<const BodyDescription> descriptions,
                                       Span<BodyHandle> out) noexcept override {
        if (out.size() < descriptions.size()) {
            return fail(ErrorCode::BufferTooSmall,
                        "jolt: the output span is smaller than the description span");
        }
        for (usize index = 0; index < descriptions.size(); ++index) {
            const Expected<BodyHandle, Error> body = create_body(world, descriptions[index]);
            if (!body) {
                for (usize undo = 0; undo < index; ++undo) {
                    (void)destroy_body(out[undo]);
                }
                return make_unexpected(body.error());
            }
            out[index] = *body;
        }
        // `physics` — "A region's collision arrives at once". One broad-phase rebuild for the whole
        // batch rather than one per body, which is the difference the requirement is about.
        if (JoltWorld* found = resolve(world); found != nullptr && !descriptions.empty()) {
            found->system.OptimizeBroadPhase();
        }
        return ok();
    }

    [[nodiscard]] Status destroy_body(BodyHandle body) noexcept override;

    [[nodiscard]] Status destroy_bodies(Span<const BodyHandle> bodies) noexcept override {
        for (const BodyHandle handle : bodies) {
            if (Status destroyed = destroy_body(handle); !destroyed) {
                return destroyed;
            }
        }
        return ok();
    }

    [[nodiscard]] Expected<BodyState, Error> body_state(BodyHandle body) const noexcept override {
        const JoltBody* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        const JoltWorld* world = worlds_[found->world];
        const JPH::BodyInterface& interface = world->system.GetBodyInterfaceNoLock();
        BodyState state;
        state.transform.translation = from_jolt(interface.GetPosition(found->id));
        state.transform.rotation = from_jolt(interface.GetRotation(found->id));
        state.linear_velocity = from_jolt(interface.GetLinearVelocity(found->id));
        state.angular_velocity = from_jolt(interface.GetAngularVelocity(found->id));
        state.motion = found->motion;
        state.asleep = found->motion == MotionType::Dynamic && !interface.IsActive(found->id);
        state.teleported = found->teleported;
        return state;
    }

    [[nodiscard]] Expected<MassProperties, Error> mass_properties(
        BodyHandle body) const noexcept override {
        const JoltBody* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        return found->mass;
    }

    [[nodiscard]] Expected<UserData, Error> body_user_data(
        BodyHandle body) const noexcept override {
        const JoltBody* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        return found->user_data;
    }

    [[nodiscard]] bool body_alive(BodyHandle body) const noexcept override {
        return resolve(body) != nullptr;
    }

    [[nodiscard]] Status set_body_transform(BodyHandle body, const Transform& transform,
                                            TeleportMode mode) noexcept override {
        JoltBody* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        JPH::BodyInterface& interface = worlds_[found->world]->system.GetBodyInterface();
        interface.SetPositionAndRotation(
            found->id,
            JPH::RVec3(transform.translation.x, transform.translation.y, transform.translation.z),
            to_jolt(transform.rotation),
            mode == TeleportMode::Teleport ? JPH::EActivation::Activate
                                           : JPH::EActivation::DontActivate);
        if (mode == TeleportMode::Teleport) {
            found->pending_teleport = true;
        }
        return ok();
    }

    [[nodiscard]] Status set_body_velocity(BodyHandle body, Vec3 linear,
                                           Vec3 angular) noexcept override {
        JoltBody* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        JPH::BodyInterface& interface = worlds_[found->world]->system.GetBodyInterface();
        // The locks are applied here as well as through `mAllowedDOFs`, because a KINEMATIC body
        // has no motion properties for Jolt to constrain and would otherwise carry a velocity out
        // of the plane the engine said it was locked to.
        interface.SetLinearAndAngularVelocity(
            found->id, to_jolt(apply_linear_locks(linear, found->locked_axes)),
            to_jolt(apply_angular_locks(angular, found->locked_axes)));
        return ok();
    }

    [[nodiscard]] Status set_body_motion_type(BodyHandle body,
                                              MotionType motion) noexcept override {
        JoltBody* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        JPH::BodyInterface& interface = worlds_[found->world]->system.GetBodyInterface();
        interface.SetMotionType(found->id, to_jolt_motion(motion),
                                motion == MotionType::Static ? JPH::EActivation::DontActivate
                                                             : JPH::EActivation::Activate);
        // The object layer carries "is this moving" in its low bit, so a motion-type change is also
        // a layer change. Forgetting this leaves a body that moves in the static broad-phase tree,
        // where it is never tested against anything.
        interface.SetObjectLayer(found->id,
                                 object_layer(found->filter.layer, motion != MotionType::Static));
        found->motion = motion;
        return ok();
    }

    [[nodiscard]] Status set_body_filter(BodyHandle body,
                                         CollisionFilter filter) noexcept override {
        JoltBody* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        found->filter = filter;
        JPH::BodyInterface& interface = worlds_[found->world]->system.GetBodyInterface();
        interface.SetObjectLayer(found->id,
                                 object_layer(filter.layer, found->motion != MotionType::Static));
        return ok();
    }

    [[nodiscard]] Status set_body_gravity_scale(BodyHandle body, f32 scale) noexcept override {
        JoltBody* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        worlds_[found->world]->system.GetBodyInterface().SetGravityFactor(found->id, scale);
        return ok();
    }

    [[nodiscard]] Status set_body_awake(BodyHandle body, bool awake) noexcept override {
        JoltBody* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        if (found->motion != MotionType::Dynamic) {
            return fail(ErrorCode::InvalidArgument,
                        "jolt: only a dynamic body sleeps, so only a dynamic body wakes");
        }
        JPH::BodyInterface& interface = worlds_[found->world]->system.GetBodyInterface();
        if (awake) {
            interface.ActivateBody(found->id);
        } else {
            interface.DeactivateBody(found->id);
        }
        return ok();
    }

    [[nodiscard]] Status set_pair_ignored(BodyHandle a, BodyHandle b,
                                          bool ignored) noexcept override {
        JoltBody* first = resolve(a);
        if (first == nullptr || resolve(b) == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        JoltWorld* world = worlds_[first->world];
        const u64 key = contact_pair_key(a, b);
        if (!ignored) {
            (void)world->ignored.remove(key);
            return ok();
        }
        const Expected<u8*, Error> inserted = world->ignored.insert(key, 1);
        return inserted ? ok() : Status{make_unexpected(inserted.error())};
    }

    [[nodiscard]] Status add_force(BodyHandle body, Vec3 force) noexcept override {
        return with_dynamic(body, [&](JPH::BodyInterface& interface, const JoltBody& record) {
            interface.AddForce(record.id, to_jolt(force));
        });
    }

    [[nodiscard]] Status add_torque(BodyHandle body, Vec3 torque) noexcept override {
        return with_dynamic(body, [&](JPH::BodyInterface& interface, const JoltBody& record) {
            interface.AddTorque(record.id, to_jolt(torque));
        });
    }

    [[nodiscard]] Status add_impulse(BodyHandle body, Vec3 impulse) noexcept override {
        return with_dynamic(body, [&](JPH::BodyInterface& interface, const JoltBody& record) {
            interface.AddImpulse(record.id,
                                 to_jolt(apply_linear_locks(impulse, record.locked_axes)));
        });
    }

    [[nodiscard]] Status add_impulse_at(BodyHandle body, Vec3 impulse,
                                        Vec3 world_point) noexcept override {
        return with_dynamic(body, [&](JPH::BodyInterface& interface, const JoltBody& record) {
            interface.AddImpulse(record.id,
                                 to_jolt(apply_linear_locks(impulse, record.locked_axes)),
                                 JPH::RVec3(world_point.x, world_point.y, world_point.z));
        });
    }

    [[nodiscard]] Status add_angular_impulse(BodyHandle body, Vec3 impulse) noexcept override {
        return with_dynamic(body, [&](JPH::BodyInterface& interface, const JoltBody& record) {
            interface.AddAngularImpulse(record.id,
                                        to_jolt(apply_angular_locks(impulse, record.locked_axes)));
        });
    }

    // --- Constraints. Not in this milestone, and it says so -------------------------------------

    [[nodiscard]] Expected<ConstraintHandle, Error> create_constraint(
        WorldHandle, const ConstraintDescription& description) noexcept override {
        // `physics` — "Unsupported feature": the capability query reports it and creation fails
        // with a clear diagnostic. Jolt HAS every constraint kind `physics` lists; what is missing
        // is the mapping, which M4's task list does not include. Saying "not implemented, here is
        // why" is the honest form — this is not a limit of the backend.
        return fail(ErrorCode::NotImplemented,
                    constraint_type_name(description.type) != nullptr
                        ? "jolt: constraints are not mapped yet (M4 delivers bodies, shapes, "
                          "events and queries); Capabilities::constraints reports false"
                        : "jolt: constraints are not mapped yet");
    }

    [[nodiscard]] Status destroy_constraint(ConstraintHandle) noexcept override {
        return fail(ErrorCode::NotImplemented, "jolt: constraints are not mapped yet");
    }

    [[nodiscard]] Status set_constraint_enabled(ConstraintHandle, bool) noexcept override {
        return fail(ErrorCode::NotImplemented, "jolt: constraints are not mapped yet");
    }

    [[nodiscard]] Status set_constraint_motor(ConstraintHandle,
                                              const MotorSettings&) noexcept override {
        return fail(ErrorCode::NotImplemented, "jolt: constraints are not mapped yet");
    }

    // --- Queries ---------------------------------------------------------------------------------

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

    // --- Used by JoltWorld's contact callbacks --------------------------------------------------

    [[nodiscard]] const JoltBody* body_record(u64 handle_bits) const noexcept {
        return resolve(BodyHandle::from_bits(handle_bits));
    }

private:
    struct MaterialSlot {
        MaterialDescription description;
        u32 generation = 0;
        bool live = false;
    };

    struct ShapeSlot {
        JPH::Ref<JPH::Shape> shape;
        ShapeType type = ShapeType::Sphere;
        u64 key = 0;
        u32 generation = 0;
        u32 references = 0;
        bool live = false;
        /// Kept so a compound can be rebuilt and a height field can be updated regionally.
        ShapeDescription description;
    };

    /// Jolt's per-body mutex count. Its own recommendation is zero, meaning "pick a default".
    static constexpr JPH::uint kBodyMutexes = 0;

    /// Named apart from the namespace-scope `to_jolt` overloads: a static member of that name would
    /// HIDE them inside this class, and every `to_jolt(Vec3)` in the file would stop compiling.
    static JPH::EMotionType to_jolt_motion(MotionType motion) noexcept {
        switch (motion) {
            case MotionType::Static:
                return JPH::EMotionType::Static;
            case MotionType::Kinematic:
                return JPH::EMotionType::Kinematic;
            case MotionType::Dynamic:
                return JPH::EMotionType::Dynamic;
        }
        return JPH::EMotionType::Static;
    }

    static JPH::EAllowedDOFs allowed_dofs(u8 locks) noexcept {
        JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
        const auto clear = [&dofs](JPH::EAllowedDOFs bit) {
            dofs = static_cast<JPH::EAllowedDOFs>(static_cast<JPH::uint8>(dofs) &
                                                  ~static_cast<JPH::uint8>(bit));
        };
        if ((locks & kLockLinearX) != 0U) {
            clear(JPH::EAllowedDOFs::TranslationX);
        }
        if ((locks & kLockLinearY) != 0U) {
            clear(JPH::EAllowedDOFs::TranslationY);
        }
        if ((locks & kLockLinearZ) != 0U) {
            clear(JPH::EAllowedDOFs::TranslationZ);
        }
        if ((locks & kLockAngularX) != 0U) {
            clear(JPH::EAllowedDOFs::RotationX);
        }
        if ((locks & kLockAngularY) != 0U) {
            clear(JPH::EAllowedDOFs::RotationY);
        }
        if ((locks & kLockAngularZ) != 0U) {
            clear(JPH::EAllowedDOFs::RotationZ);
        }
        // Jolt's own comment: `None` "is not valid and will crash. Use a static body instead." A
        // body that locked every axis is one the caller meant to be static, and turning it into an
        // all-locked dynamic body is the crash rather than the intent.
        if (static_cast<JPH::uint8>(dofs) == 0U) {
            dofs = JPH::EAllowedDOFs::TranslationX;
        }
        return dofs;
    }

    static void apply_tuning(JoltWorld& world, const Tuning& tuning) noexcept {
        JPH::PhysicsSettings settings = world.system.GetPhysicsSettings();
        settings.mNumVelocitySteps = static_cast<int>(tuning.velocity_iterations);
        settings.mNumPositionSteps = static_cast<int>(tuning.position_iterations);
        settings.mPenetrationSlop = tuning.penetration_slop;
        settings.mBaumgarte = tuning.baumgarte;
        settings.mSpeculativeContactDistance = tuning.speculative_contact_distance;
        settings.mPointVelocitySleepThreshold = tuning.sleep_linear_velocity;
        settings.mTimeBeforeSleep = tuning.time_before_sleep_seconds;
        // `sleep_angular_velocity` HAS NO JOLT ANALOGUE. Jolt decides sleep from the velocity of
        // the body's bounding sphere's extreme points, which folds rotation into one linear
        // threshold. Recorded here rather than silently dropped; `Tuning`'s comment says the engine
        // owns the names and a backend maps them.
        world.system.SetPhysicsSettings(settings);
    }

    template <class Fn>
    [[nodiscard]] Status with_dynamic(BodyHandle body, Fn&& fn) noexcept {
        JoltBody* found = resolve(body);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: no such body");
        }
        if (found->motion != MotionType::Dynamic) {
            return ok();  // a force on a static body is a no-op, not an error
        }
        fn(worlds_[found->world]->system.GetBodyInterface(), *found);
        return ok();
    }

    /// The two rejections `create_body` makes before it allocates anything: a static-only shape on
    /// a moving body, and a body whose colliders mix triggers with solids.
    [[nodiscard]] Status check_colliders(const BodyDescription& description) const noexcept;
    /// The single collider's shape, or a static compound of them.
    [[nodiscard]] Expected<JPH::Ref<JPH::Shape>, Error> body_shape(
        const BodyDescription& description) noexcept;
    /// The engine description, as Jolt's creation settings.
    [[nodiscard]] JPH::BodyCreationSettings body_settings(const BodyDescription& description,
                                                          const JoltBody& record,
                                                          const JPH::Ref<JPH::Shape>& shape,
                                                          BodyHandle handle) const noexcept;
    /// The mass properties a body ends up with. An explicit mass rescales the derived tensor rather
    /// than replacing it: the SHAPE of the inertia is the geometry's and only its magnitude is the
    /// mass's.
    [[nodiscard]] static MassProperties mass_of(const JPH::Shape& shape,
                                                f32 explicit_mass) noexcept {
        const JPH::MassProperties mass = shape.GetMassProperties();
        MassProperties out;
        out.mass = explicit_mass > 0.0f ? explicit_mass : mass.mMass;
        out.center_of_mass = from_jolt(shape.GetCenterOfMass());
        out.inertia = Vec3{mass.mInertia(0, 0), mass.mInertia(1, 1), mass.mInertia(2, 2)};
        if (explicit_mass > 0.0f && mass.mMass > 0.0f) {
            out.inertia = out.inertia * (explicit_mass / mass.mMass);
        }
        return out;
    }

    void destroy_world_slot(u32 index) noexcept;

    [[nodiscard]] JoltWorld* resolve(WorldHandle handle) noexcept {
        return const_cast<JoltWorld*>(static_cast<const JoltServer*>(this)->resolve(handle));
    }
    [[nodiscard]] const JoltWorld* resolve(WorldHandle handle) const noexcept {
        if (handle.is_null() || handle.index() >= worlds_.size()) {
            return nullptr;
        }
        const JoltWorld* world = worlds_[handle.index()];
        return (world != nullptr && world->live && world->generation == handle.generation())
                   ? world
                   : nullptr;
    }
    [[nodiscard]] JoltBody* resolve(BodyHandle handle) noexcept {
        return const_cast<JoltBody*>(static_cast<const JoltServer*>(this)->resolve(handle));
    }
    [[nodiscard]] const JoltBody* resolve(BodyHandle handle) const noexcept {
        if (handle.is_null() || handle.index() >= bodies_.size()) {
            return nullptr;
        }
        const JoltBody& body = bodies_[handle.index()];
        return (body.live && body.generation == handle.generation()) ? &body : nullptr;
    }
    [[nodiscard]] ShapeSlot* resolve(ShapeHandle handle) noexcept {
        return const_cast<ShapeSlot*>(static_cast<const JoltServer*>(this)->resolve(handle));
    }
    [[nodiscard]] const ShapeSlot* resolve(ShapeHandle handle) const noexcept {
        if (handle.is_null() || handle.index() >= shapes_.size()) {
            return nullptr;
        }
        const ShapeSlot& shape = shapes_[handle.index()];
        return (shape.live && shape.generation == handle.generation()) ? &shape : nullptr;
    }
    [[nodiscard]] MaterialSlot* resolve(MaterialHandle handle) noexcept {
        return const_cast<MaterialSlot*>(static_cast<const JoltServer*>(this)->resolve(handle));
    }
    [[nodiscard]] const MaterialSlot* resolve(MaterialHandle handle) const noexcept {
        if (handle.is_null() || handle.index() >= materials_.size()) {
            return nullptr;
        }
        const MaterialSlot& material = materials_[handle.index()];
        return (material.live && material.generation == handle.generation()) ? &material : nullptr;
    }

    /// The `ChildResolver` jolt_shapes.cpp calls for a compound's children.
    static const JPH::Shape* resolve_child(void* context, ShapeHandle child) noexcept {
        const ShapeSlot* slot = static_cast<JoltServer*>(context)->resolve(child);
        return slot == nullptr ? nullptr : slot->shape.GetPtr();
    }

    [[nodiscard]] Status reject_if_stepping() const noexcept {
        return reject_query_during_step(stepping_);
    }

    Allocator* allocator_;
    cy::jobs::JobSystem* jobs_;
    Array<JoltWorld*> worlds_;
    Array<JoltBody> bodies_;
    Array<ShapeSlot> shapes_;
    Array<MaterialSlot> materials_;
    HashMap<u64, u32> shape_cache_;
    Array<u32> free_bodies_;
    Array<u32> free_shapes_;
    Array<u32> free_materials_;
    ShapeStatistics shape_statistics_;
    bool stepping_ = false;
    bool initialized_ = false;
};

// ================================================================================================
// THE CONTACT LISTENER
// ================================================================================================

JPH::ValidateResult JoltWorld::OnContactValidate(const JPH::Body& body_a, const JPH::Body& body_b,
                                                 JPH::RVec3Arg base_offset,
                                                 const JPH::CollideShapeResult& result) {
    (void)base_offset;
    (void)result;
    const JoltBody* a = server->body_record(body_a.GetUserData());
    const JoltBody* b = server->body_record(body_b.GetUserData());
    if (a == nullptr || b == nullptr) {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }
    // The MASK half of the filter, which an object layer cannot carry — see the file header,
    // mismatch 1. The matrix half was already applied by `ObjectPairFilter` before the pair reached
    // the narrow phase.
    if (!accepts(a->filter, b->filter)) {
        return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
    }
    const BodyHandle handle_a = BodyHandle::from_bits(body_a.GetUserData());
    const BodyHandle handle_b = BodyHandle::from_bits(body_b.GetUserData());
    if (ignored.contains(contact_pair_key(handle_a, handle_b))) {
        return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
    }
    return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
}

void JoltWorld::record(const JPH::Body& body_a, const JPH::Body& body_b,
                       const JPH::ContactManifold& manifold, ContactPhase phase) noexcept {
    const JoltBody* a = server->body_record(body_a.GetUserData());
    const JoltBody* b = server->body_record(body_b.GetUserData());
    if (a == nullptr || b == nullptr) {
        return;
    }

    PendingContact contact;
    contact.a = body_a.GetUserData();
    contact.b = body_b.GetUserData();
    contact.phase = phase;
    contact.trigger = a->sensor || b->sensor;
    contact.report_stay = a->report_stay && b->report_stay;
    contact.user_data_a = a->user_data;
    contact.user_data_b = b->user_data;
    contact.normal = from_jolt(manifold.mWorldSpaceNormal);
    contact.penetration = manifold.mPenetrationDepth;
    if (!manifold.mRelativeContactPointsOn1.empty()) {
        contact.position = from_jolt(manifold.GetWorldSpaceContactPointOn1(0));
    }

    // THE IMPULSE IS AN ESTIMATE. See the file header, mismatch 3: Jolt's applied impulses live in
    // its constraint manager and this callback runs before the solver. The relative normal velocity
    // times the reduced mass is the impulse a perfectly inelastic contact WOULD apply, which is the
    // quantity an impact sound scales with and the one the threshold is written against.
    const JPH::MotionProperties* motion_a = body_a.GetMotionPropertiesUnchecked();
    const JPH::MotionProperties* motion_b = body_b.GetMotionPropertiesUnchecked();
    const f32 inverse_a = motion_a != nullptr ? motion_a->GetInverseMass() : 0.0f;
    const f32 inverse_b = motion_b != nullptr ? motion_b->GetInverseMass() : 0.0f;
    const f32 inverse_total = inverse_a + inverse_b;
    if (inverse_total > 0.0f) {
        // Read through the motion properties, NOT through `Body::GetLinearVelocity()`: that
        // accessor dereferences the motion properties, and a static body has none. Half of every
        // contact in a level is against static geometry, so the obvious spelling is a null
        // dereference on the common path — clang-analyzer found it here before a test did.
        const Vec3 velocity_a =
            motion_a != nullptr ? from_jolt(motion_a->GetLinearVelocity()) : Vec3{};
        const Vec3 velocity_b =
            motion_b != nullptr ? from_jolt(motion_b->GetLinearVelocity()) : Vec3{};
        const f32 approach = dot(velocity_b - velocity_a, contact.normal);
        contact.impulse = std::fabs(approach) / inverse_total;
    }

    const f32 threshold = a->contact_impulse_threshold > b->contact_impulse_threshold
                              ? a->contact_impulse_threshold
                              : b->contact_impulse_threshold;
    if (threshold > 0.0f && contact.impulse < threshold) {
        return;  // `physics` — "Contact filtering": below the threshold, no event
    }

    const std::lock_guard<std::mutex> guard(pending_mutex);
    (void)pending.push_back(contact);
}

void JoltWorld::OnContactAdded(const JPH::Body& body_a, const JPH::Body& body_b,
                               const JPH::ContactManifold& manifold,
                               JPH::ContactSettings& settings) {
    (void)settings;
    record(body_a, body_b, manifold, ContactPhase::Enter);
}

void JoltWorld::OnContactPersisted(const JPH::Body& body_a, const JPH::Body& body_b,
                                   const JPH::ContactManifold& manifold,
                                   JPH::ContactSettings& settings) {
    (void)settings;
    record(body_a, body_b, manifold, ContactPhase::Stay);
}

void JoltWorld::OnContactRemoved(const JPH::SubShapeIDPair& pair) {
    // The bodies may already be gone, so the pair is matched against the PREVIOUS step's record
    // rather than looked up — which is also how the exit event recovers the user data Jolt never
    // knew about.
    const std::lock_guard<std::mutex> guard(pending_mutex);
    for (const PendingContact& before : previous) {
        const JoltBody* a = server->body_record(before.a);
        const JoltBody* b = server->body_record(before.b);
        if (a == nullptr || b == nullptr) {
            continue;
        }
        const bool matches = (a->id == pair.GetBody1ID() && b->id == pair.GetBody2ID()) ||
                             (a->id == pair.GetBody2ID() && b->id == pair.GetBody1ID());
        if (!matches) {
            continue;
        }
        PendingContact exit = before;
        exit.phase = ContactPhase::Exit;
        exit.penetration = 0.0f;
        exit.impulse = 0.0f;
        (void)pending.push_back(exit);
        return;
    }
}

// ================================================================================================
// WORLDS, SHAPES AND BODIES
// ================================================================================================

void JoltServer::destroy_world_slot(u32 index) noexcept {
    if (index >= worlds_.size() || worlds_[index] == nullptr) {
        return;
    }
    JoltWorld* world = worlds_[index];
    // Bodies go with the world, exactly as in the reference backend: a body that outlived its world
    // would hold a `JPH::BodyID` into a destroyed `PhysicsSystem`.
    for (const u32 slot : world->bodies) {
        JoltBody& body = bodies_[slot];
        for (const ShapeHandle shape : body.shapes) {
            (void)destroy_shape(shape);
        }
        body.shapes.clear();
        body.live = false;
        ++body.generation;
        (void)free_bodies_.push_back(slot);
    }
    world->live = false;
    world->~JoltWorld();
    allocator_->deallocate(world, sizeof(JoltWorld), alignof(JoltWorld));
    worlds_[index] = nullptr;
}

Expected<ShapeHandle, Error> JoltServer::create_shape(
    const ShapeDescription& description) noexcept {
    if (Status valid = validate(description); !valid) {
        return make_unexpected(valid.error());
    }
    ++shape_statistics_.requests;

    // `physics` — "Shape sharing". The key is `cy::physics::shape_key()`, computed in cy_physics so
    // that this backend and the reference backend share a shape under exactly the same conditions.
    const u64 key = shape_key(description);
    if (const u32* existing = shape_cache_.find(key); existing != nullptr) {
        ShapeSlot& slot = shapes_[*existing];
        ++slot.references;
        ++shape_statistics_.cache_hits;
        return ShapeHandle::from_slot(*existing, slot.generation);
    }

    const Expected<JPH::Ref<JPH::Shape>, Error> built =
        build_shape(description, &JoltServer::resolve_child, this);
    if (!built) {
        return make_unexpected(built.error());
    }

    u32 index = 0;
    if (!free_shapes_.empty()) {
        index = free_shapes_[free_shapes_.size() - 1];
        free_shapes_.pop_back();
    } else {
        ShapeSlot fresh;
        fresh.generation = 1;
        if (Status pushed = shapes_.push_back(static_cast<ShapeSlot&&>(fresh)); !pushed) {
            return make_unexpected(pushed.error());
        }
        index = static_cast<u32>(shapes_.size() - 1);
    }
    ShapeSlot& slot = shapes_[index];
    slot.shape = *built;
    slot.type = description.type;
    slot.key = key;
    slot.live = true;
    slot.references = 1;
    // The description is kept WITHOUT its pointers: the caller's arrays do not outlive this call
    // and the Jolt shape has already copied what it needs. What is retained is the scalar half,
    // which is what a regional height-field update and a diagnostic read.
    slot.description = description;
    slot.description.points = nullptr;
    slot.description.vertices = nullptr;
    slot.description.indices = nullptr;
    slot.description.children = nullptr;
    slot.description.height_field.samples = nullptr;

    const Expected<u32*, Error> cached = shape_cache_.insert(key, index);
    if (!cached) {
        return make_unexpected(cached.error());
    }
    ++shape_statistics_.unique_shapes;
    return ShapeHandle::from_slot(index, slot.generation);
}

Status JoltServer::destroy_shape(ShapeHandle shape) noexcept {
    ShapeSlot* found = resolve(shape);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such shape");
    }
    if (found->references > 1) {
        --found->references;
        return ok();
    }
    (void)shape_cache_.remove(found->key);
    found->shape = nullptr;
    found->live = false;
    found->references = 0;
    ++found->generation;
    return free_shapes_.push_back(shape.index());
}

Status JoltServer::update_height_field(ShapeHandle shape, u32 x, u32 z, u32 width, u32 depth,
                                       Span<const f32> samples) noexcept {
    ShapeSlot* found = resolve(shape);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such shape");
    }
    if (found->type != ShapeType::HeightField) {
        return fail(ErrorCode::InvalidArgument, "jolt: the shape is not a height field");
    }
    if (samples.size() < static_cast<usize>(width) * depth) {
        return fail(ErrorCode::BufferTooSmall, "jolt: fewer samples than the region needs");
    }
    // `physics` — "A crater updates collision locally": Jolt rewrites only the blocks the region
    // touches. The temp allocator is the one the shape's own rebuild needs; a world's arena is not
    // reachable from a shape, so a small dedicated one is used and released here.
    EngineTempAllocator temp(*allocator_, 1U << 20U);
    if (!temp.valid()) {
        return fail(ErrorCode::OutOfMemory, "jolt: could not allocate the height-field scratch");
    }
    // `static_cast` and not `dynamic_cast`: the engine compiles with -fno-rtti. The cast is sound
    // because `found->type` was checked above and `create_shape` is the only thing that fills the
    // slot — a `HeightField` slot always holds a `JPH::HeightFieldShape`.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* field = static_cast<JPH::HeightFieldShape*>(found->shape.GetPtr());
    field->SetHeights(x, z, width, depth, samples.data(), static_cast<std::intptr_t>(width), temp);
    // Re-keyed for the same reason the reference backend re-keys: the cache describes CONTENTS, and
    // a deformed field must not answer a later lookup for the original.
    (void)shape_cache_.remove(found->key);
    found->key = determinism::fold_hash(found->key, 0x9E3779B97F4A7C15ULL);
    const Expected<u32*, Error> cached = shape_cache_.insert(found->key, shape.index());
    return cached ? ok() : Status{make_unexpected(cached.error())};
}

Status JoltServer::check_colliders(const BodyDescription& description) const noexcept {
    u32 triggers = 0;
    for (u32 index = 0; index < description.collider_count; ++index) {
        const ShapeSlot* shape = resolve(description.colliders[index].shape);
        if (shape == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: a collider names a shape that is not live");
        }
        if (description.motion != MotionType::Static && is_static_only(shape->type)) {
            return fail(ErrorCode::InvalidArgument,
                        "jolt: a triangle mesh, height field or plane collider may only carry a "
                        "static body; decompose the mesh into convex hulls for a dynamic one");
        }
        triggers += description.colliders[index].is_trigger ? 1U : 0U;
    }
    if (triggers != 0 && triggers != description.collider_count) {
        // See the file header, mismatch 2. Rejected rather than silently solid: a body that is
        // "half a trigger" has no representation in Jolt, and guessing which half wins is how a
        // checkpoint volume becomes a wall.
        return fail(ErrorCode::Unsupported,
                    "jolt: a body's colliders must be all triggers or all solid, because a sensor "
                    "is a property of a body here; split it into two bodies");
    }
    return ok();
}

Expected<JPH::Ref<JPH::Shape>, Error> JoltServer::body_shape(
    const BodyDescription& description) noexcept {
    // Every `resolve()` below is re-checked rather than asserted, even though `check_colliders()`
    // already proved each handle live. GCC's -Wnull-dereference reasons about this function in
    // isolation and is right to: `resolve` returns a pointer that can be null, and `CY_ASSERT` is
    // compiled out in Profile and Shipping, so an assertion is not a branch it can see.
    if (description.collider_count == 1 &&
        nearly_equal(description.colliders[0].local.rotation, Quat::identity()) &&
        length_squared(description.colliders[0].local.translation) < math::kEpsilon) {
        const ShapeSlot* only = resolve(description.colliders[0].shape);
        if (only == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: a collider names a shape that is not live");
        }
        return only->shape;
    }
    JPH::StaticCompoundShapeSettings compound;
    for (u32 index = 0; index < description.collider_count; ++index) {
        const ColliderDescription& collider = description.colliders[index];
        const ShapeSlot* child = resolve(collider.shape);
        if (child == nullptr) {
            return fail(ErrorCode::NotFound, "jolt: a collider names a shape that is not live");
        }
        compound.AddShape(to_jolt(collider.local.translation), to_jolt(collider.local.rotation),
                          child->shape.GetPtr());
    }
    const JPH::Shape::ShapeResult result = compound.Create();
    if (result.HasError()) {
        return fail(ErrorCode::InvalidArgument,
                    "jolt: the compound shape for this body was rejected");
    }
    return JPH::Ref<JPH::Shape>(result.Get());
}

JPH::BodyCreationSettings JoltServer::body_settings(const BodyDescription& description,
                                                    const JoltBody& record,
                                                    const JPH::Ref<JPH::Shape>& shape,
                                                    BodyHandle handle) const noexcept {
    JPH::BodyCreationSettings settings(
        shape,
        JPH::RVec3(description.transform.translation.x, description.transform.translation.y,
                   description.transform.translation.z),
        to_jolt(description.transform.rotation), to_jolt_motion(description.motion),
        object_layer(record.filter.layer, description.motion != MotionType::Static));
    settings.mLinearVelocity =
        to_jolt(apply_linear_locks(description.linear_velocity, description.locked_axes));
    settings.mAngularVelocity =
        to_jolt(apply_angular_locks(description.angular_velocity, description.locked_axes));
    settings.mLinearDamping = description.linear_damping;
    settings.mAngularDamping = description.angular_damping;
    settings.mGravityFactor = description.gravity_scale;
    settings.mAllowSleeping = description.allow_sleeping;
    settings.mIsSensor = record.sensor;
    settings.mUserData = handle.bits();
    settings.mMotionQuality =
        description.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    if (description.motion == MotionType::Dynamic) {
        settings.mAllowedDOFs = allowed_dofs(description.locked_axes);
    }
    if (const MaterialSlot* material = resolve(record.material); material != nullptr) {
        settings.mFriction = material->description.friction;
        settings.mRestitution = material->description.restitution;
    }
    if (description.mass > 0.0f) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = description.mass;
    }
    return settings;
}

Expected<BodyHandle, Error> JoltServer::create_body(WorldHandle world,
                                                    const BodyDescription& description) noexcept {
    JoltWorld* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such world");
    }
    if (Status valid = validate(description); !valid) {
        return make_unexpected(valid.error());
    }
    if (Status valid = check_colliders(description); !valid) {
        return make_unexpected(valid.error());
    }
    const Expected<JPH::Ref<JPH::Shape>, Error> shape = body_shape(description);
    if (!shape) {
        return make_unexpected(shape.error());
    }

    u32 index = 0;
    if (!free_bodies_.empty()) {
        index = free_bodies_[free_bodies_.size() - 1];
        free_bodies_.pop_back();
    } else {
        const Expected<JoltBody*, Error> fresh = bodies_.emplace_back(*allocator_);
        if (!fresh) {
            return make_unexpected(fresh.error());
        }
        (*fresh)->generation = 1;
        index = static_cast<u32>(bodies_.size() - 1);
    }
    JoltBody& record = bodies_[index];
    record.live = true;
    record.world = world.index();
    record.name = description.name;
    record.motion = description.motion;
    record.filter =
        description.collider_count > 0 ? description.colliders[0].filter : CollisionFilter{};
    record.material =
        description.collider_count > 0 ? description.colliders[0].material : MaterialHandle{};
    record.user_data = description.user_data;
    record.locked_axes = description.locked_axes;
    record.sensor = description.collider_count > 0 && description.colliders[0].is_trigger;
    record.report_stay = description.collider_count == 0 || description.colliders[0].report_stay;
    record.contact_impulse_threshold =
        description.collider_count > 0 ? description.colliders[0].contact_impulse_threshold : 0.0f;
    record.teleported = false;
    record.pending_teleport = false;
    record.shapes.clear();

    const BodyHandle handle = BodyHandle::from_slot(index, record.generation);
    const JPH::BodyCreationSettings settings = body_settings(description, record, *shape, handle);

    JPH::BodyInterface& interface = found->system.GetBodyInterface();
    const JPH::BodyID id = interface.CreateAndAddBody(
        settings, description.start_asleep || description.motion == MotionType::Static
                      ? JPH::EActivation::DontActivate
                      : JPH::EActivation::Activate);
    if (id.IsInvalid()) {
        record.live = false;
        (void)free_bodies_.push_back(index);
        return fail(ErrorCode::OutOfRange,
                    "jolt: the world's body capacity is full, or the body was rejected");
    }
    record.id = id;
    record.mass = mass_of(**shape, description.mass);

    for (u32 collider = 0; collider < description.collider_count; ++collider) {
        // A reference per collider, so the cache keeps the shape alive for exactly as long as some
        // body names it.
        ShapeSlot* slot = resolve(description.colliders[collider].shape);
        if (slot == nullptr) {
            continue;
        }
        ++slot->references;
        if (Status pushed = record.shapes.push_back(description.colliders[collider].shape);
            !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    if (Status pushed = found->bodies.push_back(index); !pushed) {
        interface.RemoveBody(id);
        interface.DestroyBody(id);
        record.live = false;
        (void)free_bodies_.push_back(index);
        return make_unexpected(pushed.error());
    }
    return handle;
}

Status JoltServer::destroy_body(BodyHandle body) noexcept {
    JoltBody* found = resolve(body);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such body");
    }
    JoltWorld* world = worlds_[found->world];
    if (world != nullptr) {
        JPH::BodyInterface& interface = world->system.GetBodyInterface();
        interface.RemoveBody(found->id);
        interface.DestroyBody(found->id);
        for (usize index = 0; index < world->bodies.size(); ++index) {
            if (world->bodies[index] == body.index()) {
                world->bodies.erase(index);
                break;
            }
        }
    }
    for (const ShapeHandle shape : found->shapes) {
        (void)destroy_shape(shape);
    }
    found->shapes.clear();
    found->live = false;
    ++found->generation;
    return free_bodies_.push_back(body.index());
}

// ================================================================================================
// THE STEP
// ================================================================================================

Status JoltServer::step(WorldHandle world, const StepInput& input) noexcept {
    JoltWorld* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such world");
    }
    if (!(input.delta_seconds > 0.0f) || !math::is_finite(input.delta_seconds)) {
        return fail(ErrorCode::InvalidArgument,
                    "jolt: the step must be a positive finite number of seconds");
    }
    if (stepping_) {
        return fail(ErrorCode::Unavailable, "jolt: the world is already stepping");
    }

    for (const u32 slot : found->bodies) {
        JoltBody& body = bodies_[slot];
        body.teleported = body.pending_teleport;
        body.pending_teleport = false;
    }

    {
        const std::lock_guard<std::mutex> guard(found->pending_mutex);
        found->pending.clear();
    }
    found->events.clear();

    // Diagnostics only, never hashed — the same rule the reference backend states at length.
    const auto started = std::chrono::steady_clock::now();
    stepping_ = true;
    const JPH::EPhysicsUpdateError error =
        found->system.Update(input.delta_seconds, static_cast<int>(input.collision_steps),
                             &found->temp, &found->job_system);
    stepping_ = false;
    const auto finished = std::chrono::steady_clock::now();

    if (error != JPH::EPhysicsUpdateError::None) {
        // Jolt reports a capacity overflow rather than corrupting the simulation. Surfaced as an
        // error naming the world's own configuration knobs, because that is what has to change.
        return fail(ErrorCode::OutOfRange,
                    "jolt: the step overflowed a buffer — raise body_pair_capacity or "
                    "contact_constraint_capacity on the world",
                    static_cast<i64>(error));
    }

    // --- The deterministic order ---------------------------------------------------------------
    //
    // The contacts arrived on worker threads in whatever order they finished. Sorting by
    // (a, b, phase) makes the event buffer a function of the SIMULATION rather than of the
    // scheduler — see the file header, mismatch 4. Insertion sort: a step's event count is tens,
    // and a sort that allocates would put an allocation back on the step path.
    {
        const std::lock_guard<std::mutex> guard(found->pending_mutex);
        for (usize index = 1; index < found->pending.size(); ++index) {
            const PendingContact key = found->pending[index];
            usize slot = index;
            while (slot > 0) {
                const PendingContact& previous_contact = found->pending[slot - 1];
                const bool after =
                    previous_contact.a > key.a ||
                    (previous_contact.a == key.a &&
                     (previous_contact.b > key.b ||
                      (previous_contact.b == key.b &&
                       static_cast<u8>(previous_contact.phase) > static_cast<u8>(key.phase))));
                if (!after) {
                    break;
                }
                found->pending[slot] = found->pending[slot - 1];
                --slot;
            }
            found->pending[slot] = key;
        }

        for (usize index = 0; index < found->pending.size(); ++index) {
            const PendingContact& contact = found->pending[index];
            if (contact.phase == ContactPhase::Stay && !contact.report_stay) {
                continue;
            }
            ContactEvent event;
            event.a = BodyHandle::from_bits(contact.a);
            event.b = BodyHandle::from_bits(contact.b);
            event.user_data_a = contact.user_data_a;
            event.user_data_b = contact.user_data_b;
            event.phase = contact.phase;
            event.trigger = contact.trigger;
            event.point_count = contact.phase == ContactPhase::Exit ? 0U : 1U;
            event.points[0].position = contact.position;
            event.points[0].normal = contact.normal;
            event.points[0].penetration = contact.penetration;
            event.points[0].normal_impulse = contact.impulse;
            event.total_impulse = contact.impulse;
            (void)found->events.push(event);
        }

        // This step's contacts become the next step's "previous", so `OnContactRemoved` can name
        // bodies whose properties Jolt has already forgotten.
        Array<PendingContact> swap = static_cast<Array<PendingContact>&&>(found->previous);
        found->previous = static_cast<Array<PendingContact>&&>(found->pending);
        found->pending = static_cast<Array<PendingContact>&&>(swap);
    }

    found->statistics.body_count = found->system.GetNumBodies();
    found->statistics.active_body_count =
        found->system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    found->statistics.contact_count = found->events.size();
    found->statistics.constraint_count = 0;
    found->statistics.island_count = 0;
    found->statistics.tick = input.tick;
    // ONE TIMER, NOT THREE, AND SAYING SO. `physics`' "Diagnosing a slow step" wants the cost split
    // across broad phase, narrow phase and solve; Jolt does not publish that split without its own
    // profiler, which cmake/dependencies.cmake deliberately leaves off (it is a second timeline
    // beside the engine's trace). The total is real and the three parts are zero rather than
    // fabricated — a made-up split is worse than an absent one.
    found->statistics.broad_phase_ns = 0;
    found->statistics.narrow_phase_ns = 0;
    found->statistics.solve_ns = 0;
    found->statistics.total_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
    return ok();
}

// ================================================================================================
// QUERIES
// ================================================================================================

namespace {

/// The matrix half of the engine's filter, as Jolt's object-layer query filter.
class QueryLayerFilter final : public JPH::ObjectLayerFilter {
public:
    QueryLayerFilter(const CollisionMatrix& matrix, CollisionFilter filter) noexcept
        : matrix_(&matrix), filter_(filter) {}

    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return matrix_->allows(filter_.layer, engine_layer(layer));
    }

private:
    const CollisionMatrix* matrix_;
    CollisionFilter filter_;
};

/// Everything else the engine's `QueryFilter` says, applied where the body is in hand: the mutual
/// mask rule, the ignore list, the motion-type switches and whether sensors are included.
class QueryBodyFilter final : public JPH::BodyFilter {
public:
    QueryBodyFilter(const JoltServer& server, const QueryFilter& filter) noexcept
        : server_(&server), filter_(&filter) {}

    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        const JoltBody* record = server_->body_record(body.GetUserData());
        if (record == nullptr) {
            return false;
        }
        if (!filter_->includes(record->motion)) {
            return false;
        }
        if (record->sensor && !filter_->include_triggers) {
            return false;
        }
        if (filter_->ignores(BodyHandle::from_bits(body.GetUserData()))) {
            return false;
        }
        return accepts(filter_->filter, record->filter);
    }

private:
    const JoltServer* server_;
    const QueryFilter* filter_;
};

}  // namespace

Expected<RayCastHit, Error> JoltServer::raycast(WorldHandle world, const RayCastInput& input,
                                                const QueryFilter& filter) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const JoltWorld* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such world");
    }
    const Vec3 direction = normalize(input.direction);
    const JPH::RRayCast ray{JPH::RVec3(input.origin.x, input.origin.y, input.origin.z),
                            to_jolt(direction * input.max_distance)};
    JPH::RayCastResult result;
    const QueryLayerFilter layers(found->description.matrix, filter.filter);
    const QueryBodyFilter bodies(*this, filter);
    RayCastHit hit;
    if (!found->system.GetNarrowPhaseQuery().CastRay(ray, result, JPH::BroadPhaseLayerFilter(),
                                                     layers, bodies)) {
        return hit;  // no hit is not an error; the body handle is null
    }

    const JPH::BodyLockRead lock(found->system.GetBodyLockInterface(), result.mBodyID);
    if (!lock.Succeeded()) {
        return hit;
    }
    const JPH::Body& body = lock.GetBody();
    const JoltBody* record = body_record(body.GetUserData());
    hit.body = BodyHandle::from_bits(body.GetUserData());
    hit.user_data = record != nullptr ? record->user_data : 0;
    hit.distance = result.mFraction * input.max_distance;
    hit.position = input.origin + direction * hit.distance;
    hit.normal = from_jolt(body.GetWorldSpaceSurfaceNormal(
        result.mSubShapeID2, JPH::RVec3(hit.position.x, hit.position.y, hit.position.z)));
    hit.material = record != nullptr ? record->material : MaterialHandle{};
    hit.trigger = body.IsSensor();
    return hit;
}

Expected<u32, Error> JoltServer::raycast_all(WorldHandle world, const RayCastInput& input,
                                             const QueryFilter& filter,
                                             Span<RayCastHit> out) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const JoltWorld* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such world");
    }
    const Vec3 direction = normalize(input.direction);
    const JPH::RRayCast ray{JPH::RVec3(input.origin.x, input.origin.y, input.origin.z),
                            to_jolt(direction * input.max_distance)};
    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = filter.cull_back_faces
                                          ? JPH::EBackFaceMode::IgnoreBackFaces
                                          : JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const QueryLayerFilter layers(found->description.matrix, filter.filter);
    const QueryBodyFilter bodies(*this, filter);
    found->system.GetNarrowPhaseQuery().CastRay(ray, settings, collector,
                                                JPH::BroadPhaseLayerFilter(), layers, bodies);
    collector.Sort();  // by fraction, which is by distance — `physics` asks for sorted hits

    u32 count = 0;
    for (const JPH::RayCastResult& result : collector.mHits) {
        if (count >= out.size()) {
            break;
        }
        const JPH::BodyLockRead lock(found->system.GetBodyLockInterface(), result.mBodyID);
        if (!lock.Succeeded()) {
            continue;
        }
        const JPH::Body& body = lock.GetBody();
        const JoltBody* record = body_record(body.GetUserData());
        RayCastHit hit;
        hit.body = BodyHandle::from_bits(body.GetUserData());
        hit.user_data = record != nullptr ? record->user_data : 0;
        hit.distance = result.mFraction * input.max_distance;
        hit.position = input.origin + direction * hit.distance;
        hit.normal = from_jolt(body.GetWorldSpaceSurfaceNormal(
            result.mSubShapeID2, JPH::RVec3(hit.position.x, hit.position.y, hit.position.z)));
        hit.material = record != nullptr ? record->material : MaterialHandle{};
        hit.trigger = body.IsSensor();
        out[count++] = hit;
    }
    return count;
}

Expected<ShapeCastHit, Error> JoltServer::shape_cast(WorldHandle world, const ShapeCastInput& input,
                                                     const QueryFilter& filter) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const JoltWorld* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such world");
    }
    const ShapeSlot* shape = resolve(input.shape);
    if (shape == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such shape");
    }
    const Vec3 direction = normalize(input.direction);
    const JPH::RMat44 start = JPH::RMat44::sRotationTranslation(
        to_jolt(input.start.rotation),
        JPH::RVec3(input.start.translation.x, input.start.translation.y,
                   input.start.translation.z));
    const JPH::RShapeCast cast(shape->shape.GetPtr(), JPH::Vec3::sOne(), start,
                               to_jolt(direction * input.max_distance));
    JPH::ShapeCastSettings settings;
    // Reported rather than skipped: a controller that starts inside geometry has to depenetrate,
    // and a sweep that silently ignored the overlap would let it slide out through the wall.
    settings.mReturnDeepestPoint = true;
    settings.mBackFaceModeTriangles = filter.cull_back_faces
                                          ? JPH::EBackFaceMode::IgnoreBackFaces
                                          : JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const QueryLayerFilter layers(found->description.matrix, filter.filter);
    const QueryBodyFilter bodies(*this, filter);
    found->system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector,
                                                  JPH::BroadPhaseLayerFilter(), layers, bodies);

    ShapeCastHit hit;
    if (!collector.HadHit()) {
        return hit;
    }
    const JPH::BodyLockRead lock(found->system.GetBodyLockInterface(), collector.mHit.mBodyID2);
    if (!lock.Succeeded()) {
        return hit;
    }
    const JPH::Body& body = lock.GetBody();
    const JoltBody* record = body_record(body.GetUserData());
    hit.body = BodyHandle::from_bits(body.GetUserData());
    hit.user_data = record != nullptr ? record->user_data : 0;
    hit.distance = collector.mHit.mFraction * input.max_distance;
    hit.fraction = collector.mHit.mFraction;
    hit.position = from_jolt(collector.mHit.mContactPointOn2);
    // Jolt's penetration axis points INTO body 2. The engine's convention is a normal pointing out
    // of the surface that was hit, which is the direction a caller pushes itself along.
    hit.normal = normalize(-from_jolt(collector.mHit.mPenetrationAxis));
    hit.material = record != nullptr ? record->material : MaterialHandle{};
    hit.trigger = body.IsSensor();
    hit.started_penetrating = collector.mHit.mFraction <= 0.0f;
    return hit;
}

Expected<u32, Error> JoltServer::overlap(WorldHandle world, const OverlapInput& input,
                                         const QueryFilter& filter,
                                         Span<OverlapHit> out) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const JoltWorld* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such world");
    }
    const ShapeSlot* shape = resolve(input.shape);
    if (shape == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such shape");
    }
    const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(
        to_jolt(input.transform.rotation),
        JPH::RVec3(input.transform.translation.x, input.transform.translation.y,
                   input.transform.translation.z));
    const JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const QueryLayerFilter layers(found->description.matrix, filter.filter);
    const QueryBodyFilter bodies(*this, filter);
    found->system.GetNarrowPhaseQuery().CollideShape(
        shape->shape.GetPtr(), JPH::Vec3::sOne(), transform, settings, JPH::RVec3::sZero(),
        collector, JPH::BroadPhaseLayerFilter(), layers, bodies);

    u32 count = 0;
    for (const JPH::CollideShapeResult& result : collector.mHits) {
        if (count >= out.size()) {
            break;
        }
        const JPH::BodyLockRead lock(found->system.GetBodyLockInterface(), result.mBodyID2);
        if (!lock.Succeeded()) {
            continue;
        }
        const JPH::Body& body = lock.GetBody();
        const BodyHandle handle = BodyHandle::from_bits(body.GetUserData());
        bool duplicate = false;
        for (u32 index = 0; index < count; ++index) {
            duplicate = duplicate || out[index].body == handle;
        }
        if (duplicate) {
            continue;  // one hit per body: a caller asked which bodies overlap, not which shapes
        }
        const JoltBody* record = body_record(body.GetUserData());
        OverlapHit overlap_hit;
        overlap_hit.body = handle;
        overlap_hit.user_data = record != nullptr ? record->user_data : 0;
        overlap_hit.trigger = body.IsSensor();
        out[count++] = overlap_hit;
    }
    return count;
}

Expected<u32, Error> JoltServer::overlap_point(WorldHandle world, Vec3 point,
                                               const QueryFilter& filter,
                                               Span<OverlapHit> out) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const JoltWorld* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such world");
    }
    JPH::AllHitCollisionCollector<JPH::CollidePointCollector> collector;
    const QueryLayerFilter layers(found->description.matrix, filter.filter);
    const QueryBodyFilter bodies(*this, filter);
    found->system.GetNarrowPhaseQuery().CollidePoint(JPH::RVec3(point.x, point.y, point.z),
                                                     collector, JPH::BroadPhaseLayerFilter(),
                                                     layers, bodies);
    u32 count = 0;
    for (const JPH::CollidePointResult& result : collector.mHits) {
        if (count >= out.size()) {
            break;
        }
        const JPH::BodyLockRead lock(found->system.GetBodyLockInterface(), result.mBodyID);
        if (!lock.Succeeded()) {
            continue;
        }
        const JPH::Body& body = lock.GetBody();
        const JoltBody* record = body_record(body.GetUserData());
        OverlapHit overlap_hit;
        overlap_hit.body = BodyHandle::from_bits(body.GetUserData());
        overlap_hit.user_data = record != nullptr ? record->user_data : 0;
        overlap_hit.trigger = body.IsSensor();
        out[count++] = overlap_hit;
    }
    return count;
}

Expected<ClosestPoint, Error> JoltServer::closest_point(WorldHandle world,
                                                        const ClosestPointInput& input,
                                                        const QueryFilter& filter) const noexcept {
    if (Status allowed = reject_if_stepping(); !allowed) {
        return make_unexpected(allowed.error());
    }
    const JoltWorld* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such world");
    }
    // A point has no shape, so the query is a zero-radius sphere with a maximum separation. That is
    // Jolt's own way of asking "what is near here", and it keeps the answer exact rather than
    // approximating with a large sphere whose surface would bias the distance.
    const JPH::SphereShape probe(math::kEpsilon);
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = input.max_distance;
    JPH::ClosestHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const QueryLayerFilter layers(found->description.matrix, filter.filter);
    const QueryBodyFilter bodies(*this, filter);
    found->system.GetNarrowPhaseQuery().CollideShape(
        &probe, JPH::Vec3::sOne(),
        JPH::RMat44::sTranslation(JPH::RVec3(input.point.x, input.point.y, input.point.z)),
        settings, JPH::RVec3::sZero(), collector, JPH::BroadPhaseLayerFilter(), layers, bodies);

    ClosestPoint closest;
    if (!collector.HadHit()) {
        return closest;
    }
    const JPH::BodyLockRead lock(found->system.GetBodyLockInterface(), collector.mHit.mBodyID2);
    if (!lock.Succeeded()) {
        return closest;
    }
    const JPH::Body& body = lock.GetBody();
    const JoltBody* record = body_record(body.GetUserData());
    closest.body = BodyHandle::from_bits(body.GetUserData());
    closest.user_data = record != nullptr ? record->user_data : 0;
    closest.position = from_jolt(collector.mHit.mContactPointOn2);
    closest.normal = normalize(-from_jolt(collector.mHit.mPenetrationAxis));
    // Jolt reports a NEGATIVE penetration depth when the shapes are apart, which is the separation.
    closest.distance = math::max(0.0f, -collector.mHit.mPenetrationDepth);
    closest.material = record != nullptr ? record->material : MaterialHandle{};
    return closest;
}

// ================================================================================================
// DETERMINISM AND DEBUG DRAW
// ================================================================================================

Status JoltServer::hash_state(WorldHandle world, determinism::StateHashTree& tree) const noexcept {
    const JoltWorld* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such world");
    }
    if (Status opened = tree.begin(determinism::HashLevel::World, world.bits(), "physics");
        !opened) {
        return opened;
    }
    // CREATION ORDER, from the engine's own array — not `PhysicsSystem::GetBodies()`, whose order
    // is the body manager's and is not part of Jolt's contract. Two runs must agree, and the order
    // they agree on has to be one this engine controls.
    const JPH::BodyInterface& interface = found->system.GetBodyInterfaceNoLock();
    for (const u32 slot : found->bodies) {
        const JoltBody& body = bodies_[slot];
        const BodyHandle handle = BodyHandle::from_slot(slot, body.generation);
        if (Status opened = tree.begin(determinism::HashLevel::Entity, handle.bits(), "body");
            !opened) {
            return opened;
        }
        const Vec3 position = from_jolt(interface.GetPosition(body.id));
        const Quat rotation = from_jolt(interface.GetRotation(body.id));
        const Vec3 linear = from_jolt(interface.GetLinearVelocity(body.id));
        const Vec3 angular = from_jolt(interface.GetAngularVelocity(body.id));
        tree.mix_f32(position.x);
        tree.mix_f32(position.y);
        tree.mix_f32(position.z);
        tree.mix_f32(rotation.x);
        tree.mix_f32(rotation.y);
        tree.mix_f32(rotation.z);
        tree.mix_f32(rotation.w);
        tree.mix_f32(linear.x);
        tree.mix_f32(linear.y);
        tree.mix_f32(linear.z);
        tree.mix_f32(angular.x);
        tree.mix_f32(angular.y);
        tree.mix_f32(angular.z);
        tree.mix_u64(static_cast<u64>(body.motion));
        tree.mix_u64(interface.IsActive(body.id) ? 0U : 1U);
        if (Status closed = tree.end(); !closed) {
            return closed;
        }
    }
    return tree.end();
}

Status JoltServer::debug_draw(WorldHandle world, DebugDrawFlags flags,
                              DebugDrawSink& sink) const noexcept {
    const JoltWorld* found = resolve(world);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "jolt: no such world");
    }
    // Jolt's own debug renderer is deliberately not built (cmake/dependencies.cmake: it is a second
    // draw path beside `cy::physics::DebugDrawSink`). What is drawn here is what the engine's own
    // vocabulary can express from the simulated state: bounds, centres of mass and velocities, plus
    // this step's contacts — which is `physics`' "Colliders do not match visuals" for the parts a
    // sink can render without knowing a Jolt shape.
    const JPH::BodyInterface& interface = found->system.GetBodyInterfaceNoLock();
    for (const u32 slot : found->bodies) {
        const JoltBody& body = bodies_[slot];
        const DebugColor color = body_color(body.motion, body.sensor, !interface.IsActive(body.id));
        const Vec3 position = from_jolt(interface.GetPosition(body.id));
        if (has_flag(flags, DebugDrawFlags::Colliders) ||
            has_flag(flags, DebugDrawFlags::BroadPhaseBounds)) {
            const JPH::RefConst<JPH::Shape> shape = interface.GetShape(body.id);
            if (shape != nullptr) {
                const JPH::AABox local = shape->GetLocalBounds();
                const Aabb bounds =
                    Aabb::from_min_max(from_jolt(local.mMin), from_jolt(local.mMax));
                sink.box(bounds,
                         Transform{from_jolt(interface.GetRotation(body.id)), position,
                                   Vec3{1.0f, 1.0f, 1.0f}},
                         color);
            }
        }
        if (has_flag(flags, DebugDrawFlags::CentersOfMass)) {
            sink.sphere(from_jolt(interface.GetCenterOfMassPosition(body.id)), 0.02f, color);
        }
        if (has_flag(flags, DebugDrawFlags::Velocities)) {
            sink.line(position, position + from_jolt(interface.GetLinearVelocity(body.id)),
                      DebugColor::Velocity);
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

Expected<PhysicsServer*, Error> create_server(Allocator& allocator,
                                              cy::jobs::JobSystem* jobs) noexcept {
    void* storage = allocator.allocate(sizeof(JoltServer), alignof(JoltServer));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "jolt: could not allocate the backend");
    }
    return construct_at<JoltServer>(storage, allocator, jobs);
}

void destroy_server(PhysicsServer* server, Allocator& allocator) noexcept {
    if (server == nullptr) {
        return;
    }
    // `static_cast` and not `dynamic_cast`: the engine compiles with -fno-rtti, so `dynamic_cast`
    // does not exist here. The cast is sound because `create_server` above is the only thing that
    // produces one of these pointers and it always produces a `JoltServer`.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* concrete = static_cast<JoltServer*>(server);
    concrete->~JoltServer();
    allocator.deallocate(concrete, sizeof(JoltServer), alignof(JoltServer));
}

}  // namespace cy::physics::jolt
