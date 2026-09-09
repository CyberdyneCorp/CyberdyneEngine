// The physics ECS bridge. See cy/physics/bridge.h for the argument; this file is the four steps
// samples/04-character's host performs longhand.

#include <cy/ecs/query.h>
#include <cy/physics/bridge.h>

namespace cy::physics {
namespace {

/// The one place an entity becomes a body's `user_data`, and back.
///
/// `physics`' `UserData` is "64 bits the caller owns the meaning of" and the caller is this file.
/// A second spelling of the conversion elsewhere is how a contact event ends up naming the wrong
/// entity, so both directions live here and nothing else performs the cast.
[[nodiscard]] constexpr UserData user_data_of(ecs::Entity entity) noexcept {
    return static_cast<UserData>(entity.bits());
}

[[nodiscard]] constexpr ecs::Entity entity_of(UserData user_data) noexcept {
    return ecs::Entity::from_bits(static_cast<u64>(user_data));
}

}  // namespace

PhysicsBridge::PhysicsBridge(Allocator& allocator, scene::SceneTree& tree,
                             const PhysicsComponents& components, PhysicsServer& server,
                             WorldHandle physics_world) noexcept
    : world_(&tree.world()),
      tree_(&tree),
      scene_(tree.components()),
      components_(components),
      server_(&server),
      physics_world_(physics_world),
      stepper_(server, physics_world, allocator),
      index_(allocator),
      tracked_(allocator),
      shapes_(allocator),
      doomed_(allocator),
      colliders_(allocator),
      pending_(allocator) {}

PhysicsBridge::~PhysicsBridge() {
    teardown();
}

// --- Creating bodies -----------------------------------------------------------------------------

Transform PhysicsBridge::placement_of(ecs::Entity entity) const noexcept {
    // The world placement when propagation has produced one, because a body lives in world space
    // and a child node's `LocalTransform` is relative to its parent. A world with no propagation
    // run yet has an identity `WorldTransform` on every node, which would put every body at the
    // origin — so the authored placement is the fallback, and for a root node the two are equal by
    // propagation's own assertion.
    if (const auto* world = world_->get<scene::WorldTransform>(entity, scene_.world_transform);
        world != nullptr && world->value != Transform::identity()) {
        return world->value;
    }
    if (const auto* local = world_->get<scene::LocalTransform>(entity, scene_.local_transform);
        local != nullptr) {
        return local->value;
    }
    return Transform::identity();
}

Status PhysicsBridge::collect_colliders(ecs::Entity entity, Array<ColliderDescription>& out,
                                        u32& first_shape) noexcept {
    out.clear();
    first_shape = static_cast<u32>(shapes_.size());

    const auto* material = world_->get<PhysicsMaterial>(entity, components_.material);

    // A `Collider` and a `Trigger` on one entity are two colliders on one body, one of them a
    // sensor — which is exactly what `ColliderDescription::is_trigger` is for, and is why a trigger
    // is a component rather than a flag: the ECS answers "which entities are triggers" without
    // reading a field.
    if (auto* collider = world_->get_mut<Collider>(entity, components_.collider);
        collider != nullptr) {
        const Expected<ShapeHandle, Error> shape = server_->create_shape(collider->shape);
        if (!shape) {
            return make_unexpected(shape.error());
        }
        collider->handle = *shape;
        if (Status kept = shapes_.push_back(*shape); !kept) {
            (void)server_->destroy_shape(*shape);
            return kept;
        }
        ++statistics_.shapes_created;

        ColliderDescription description;
        description.shape = *shape;
        description.local = collider->local;
        description.material = (material != nullptr) ? material->handle : collider->material;
        description.filter = collider->filter;
        description.contact_impulse_threshold = collider->contact_impulse_threshold;
        description.report_stay = collider->report_stay;
        if (Status added = out.push_back(description); !added) {
            return added;
        }
    }

    if (auto* trigger = world_->get_mut<Trigger>(entity, components_.trigger); trigger != nullptr) {
        const Expected<ShapeHandle, Error> shape = server_->create_shape(trigger->shape);
        if (!shape) {
            return make_unexpected(shape.error());
        }
        trigger->handle = *shape;
        if (Status kept = shapes_.push_back(*shape); !kept) {
            (void)server_->destroy_shape(*shape);
            return kept;
        }
        ++statistics_.shapes_created;

        ColliderDescription description;
        description.shape = *shape;
        description.local = trigger->local;
        description.filter = trigger->filter;
        description.is_trigger = true;
        if (Status added = out.push_back(description); !added) {
            return added;
        }
    }
    return ok();
}

Status PhysicsBridge::create_for(ecs::Entity entity, ComponentTypeId source) noexcept {
    u32 first_shape = 0;
    if (Status collected = collect_colliders(entity, colliders_, first_shape); !collected) {
        return collected;
    }
    const u32 shape_count = static_cast<u32>(shapes_.size()) - first_shape;

    const Transform placement = placement_of(entity);
    const Span<const ColliderDescription> colliders(colliders_.data(), colliders_.size());
    const UserData user_data = user_data_of(entity);

    BodyDescription description;
    if (source == components_.rigid_body) {
        const auto* rigid = world_->get<RigidBody>(entity, components_.rigid_body);
        description =
            body_from(rigid != nullptr ? *rigid : RigidBody{}, placement, colliders, user_data);
    } else if (source == components_.static_body) {
        description = static_body_from(placement, colliders, user_data);
    } else {
        description = kinematic_body_from(placement, colliders, user_data);
    }

    const Expected<BodyHandle, Error> created = server_->create_body(physics_world_, description);
    if (!created) {
        // ONE ENTITY'S REFUSAL DOES NOT FAIL THE WORLD — see BridgeStatistics::bodies_refused. The
        // shapes it created are released here, because nothing will hold their range.
        for (usize index = shapes_.size(); index > first_shape; --index) {
            (void)server_->destroy_shape(shapes_[index - 1]);
            shapes_.pop_back();
        }
        ++statistics_.bodies_refused;
        last_error_ = make_unexpected(created.error());
        return ok();
    }

    Tracked tracked;
    tracked.entity = entity;
    tracked.body = *created;
    tracked.source = source;
    tracked.first_shape = first_shape;
    tracked.shape_count = shape_count;
    if (Status kept = tracked_.push_back(tracked); !kept) {
        (void)server_->destroy_body(*created);
        return kept;
    }
    const Expected<u32*, Error> mapped =
        index_.insert(entity.bits(), static_cast<u32>(tracked_.size() - 1));
    if (!mapped) {
        (void)server_->destroy_body(*created);
        tracked_.pop_back();
        return make_unexpected(mapped.error());
    }
    if (Status tracking = stepper_.track(*created); !tracking) {
        return tracking;
    }
    ++statistics_.bodies_created;

    // The handle goes back into the component, which is what `BodyRef`'s note means by "null until
    // the bridge creates it". A system that wants to push a body reads it from there rather than
    // asking this class.
    if (source == components_.rigid_body) {
        if (auto* rigid = world_->get_mut<RigidBody>(entity, components_.rigid_body);
            rigid != nullptr) {
            rigid->body = *created;
        }
    } else if (source == components_.static_body) {
        if (auto* body = world_->get_mut<StaticBody>(entity, components_.static_body);
            body != nullptr) {
            body->body = *created;
        }
    } else if (auto* body = world_->get_mut<KinematicBody>(entity, components_.kinematic_body);
               body != nullptr) {
        body->body = *created;
    }
    return ok();
}

// --- The sweep -----------------------------------------------------------------------------------

void PhysicsBridge::release(Tracked& tracked) noexcept {
    stepper_.untrack(tracked.body);
    (void)server_->destroy_body(tracked.body);
    ++statistics_.bodies_destroyed;
    // The shapes stay in `shapes_` until teardown: a range removed from the middle would renumber
    // every `first_shape` above it, and the alternative — a handle per tracked body — costs an
    // allocation per body to save a handful of slots per world. `teardown()` destroys them all.
    tracked.body = BodyHandle();
}

Status PhysicsBridge::sweep_removed() noexcept {
    doomed_.clear();
    for (usize index = 0; index < tracked_.size(); ++index) {
        const Tracked& tracked = tracked_[index];
        const bool alive = world_->is_alive(tracked.entity);
        const bool still_authored = alive && world_->has(tracked.entity, tracked.source);
        if (alive && still_authored) {
            continue;
        }
        if (Status noted = doomed_.push_back(static_cast<u32>(index)); !noted) {
            return noted;
        }
    }
    // Highest index first, so a swap-and-pop never moves an entry this loop has yet to reach.
    for (usize step = doomed_.size(); step > 0; --step) {
        const u32 index = doomed_[step - 1];
        release(tracked_[index]);
        (void)index_.remove(tracked_[index].entity.bits());
        const u32 last = static_cast<u32>(tracked_.size() - 1);
        if (index != last) {
            tracked_[index] = tracked_[last];
            if (u32* moved = index_.find(tracked_[index].entity.bits()); moved != nullptr) {
                *moved = index;
            }
        }
        tracked_.pop_back();
    }
    return ok();
}

// --- The three entry points ----------------------------------------------------------------------

Status PhysicsBridge::sync() noexcept {
    if (torn_down_) {
        return fail(ErrorCode::Unavailable, "this physics bridge has been torn down");
    }
    if (Status swept = sweep_removed(); !swept) {
        return swept;
    }

    pending_.clear();
    const ComponentTypeId sources[3] = {components_.rigid_body, components_.static_body,
                                        components_.kinematic_body};
    for (const ComponentTypeId source : sources) {
        ecs::QueryDesc desc(world_->allocator());
        if (Status declared = desc.with(source); !declared) {
            return declared;
        }
        ecs::Query query(*world_, std::move(desc));
        Status collecting = ok();
        Status walked = query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
            for (const ecs::Entity entity : chunk.entities()) {
                if (!collecting || index_.contains(entity.bits())) {
                    continue;
                }
                // An entity carrying two body components is answered once, by precedence, rather
                // than getting two bodies at one placement.
                if (components_.body_component_of(*world_, entity) != source) {
                    continue;
                }
                Pending pending;
                pending.entity = entity;
                pending.source = source;
                collecting = pending_.push_back(pending);
            }
        });
        if (!walked) {
            return walked;
        }
        if (!collecting) {
            return collecting;
        }
    }

    // OUTSIDE THE WALK. `create_for` writes the body handle back into the component it read, and a
    // write into a chunk the query is iterating is the shape of defect that shows up as a body at
    // the origin three archetypes later.
    for (const Pending& waiting : pending_) {
        if (Status made = create_for(waiting.entity, waiting.source); !made) {
            return made;
        }
    }

    // The two components no backend maps yet, counted so the gap is a number rather than a silence.
    statistics_.joints_deferred = 0;
    statistics_.characters_deferred = 0;
    const ComponentTypeId deferred[2] = {components_.joint, components_.character_body};
    for (u32 which = 0; which < 2; ++which) {
        ecs::QueryDesc desc(world_->allocator());
        if (Status declared = desc.with(deferred[which]); !declared) {
            return declared;
        }
        ecs::Query query(*world_, std::move(desc));
        u64 seen = 0;
        Status walked = query.for_each_chunk(
            [&](ecs::QueryChunk& chunk) noexcept { seen += chunk.entities().size(); });
        if (!walked) {
            return walked;
        }
        (which == 0 ? statistics_.joints_deferred : statistics_.characters_deferred) = seen;
    }
    return ok();
}

Status PhysicsBridge::step(const determinism::SimulationClock& clock) noexcept {
    if (torn_down_) {
        return fail(ErrorCode::Unavailable, "this physics bridge has been torn down");
    }
    if (Status stepped = stepper_.tick(clock, this); !stepped) {
        return stepped;
    }
    statistics_.steps = stepper_.steps();
    return ok();
}

Status PhysicsBridge::advance(const determinism::SimulationClock& clock) noexcept {
    last_error_ = ok();
    if (Status synced = sync(); !synced) {
        last_error_ = synced;
        return synced;
    }
    if (Status stepped = step(clock); !stepped) {
        last_error_ = stepped;
        return stepped;
    }
    return ok();
}

Expected<ecs::SystemId, Error> PhysicsBridge::install(
    ecs::Schedule& schedule, const determinism::SimulationClock& clock) noexcept {
    clock_ = &clock;

    ecs::SystemDesc desc;
    desc.name = "cy::physics::PhysicsBridge";
    desc.user = this;
    desc.body = [](const ecs::SystemContext& context) noexcept {
        auto* bridge = static_cast<PhysicsBridge*>(context.user);
        if (bridge == nullptr || bridge->clock_ == nullptr) {
            return;
        }
        // The result is kept rather than dropped: a system body cannot return one, and a host that
        // never noticed a body failing to be created would be a host whose world silently stopped
        // matching its document. `last_error()` is where it is read back.
        (void)bridge->advance(*bridge->clock_);
    };

    // THE DECLARATION IS EVERY COMPONENT THIS SYSTEM TOUCHES, AND THE ACCESS IT REALLY TAKES.
    //
    // `ecs/system.h`'s header states the one condition the access model rests on: "the query and
    // the declaration are the same object", because "a system that writes down its access
    // separately from the query it runs can drift, and nothing catches the drift". This system is
    // the case that cannot satisfy it — it does not run one query, it runs five and then calls into
    // a server — so the declaration IS written separately, and the only defence is that it is
    // written from the list of what the code does rather than from what it is mostly about.
    //
    // WRITES, and each one is a real write:
    //   LocalTransform        `publish` writes the stepped placement into it
    //   NodeState             `mark_transform_changed` sets the dirty bits, on the node AND its
    //                         ancestors — which is why a system running beside this one that also
    //                         marked a transform must be ordered against it
    //   RigidBody, StaticBody, KinematicBody, Collider, Trigger
    //                         the handle the server returned goes back into the component
    // READS:
    //   WorldTransform        the placement a body is created at, and the parent's frame `publish`
    //                         undoes to get back to a local placement
    //   PhysicsMaterial, Joint, CharacterBody
    //                         read, and counted, and not written
    const ComponentTypeId writes[7] = {scene_.local_transform,     scene_.state,
                                       components_.rigid_body,     components_.static_body,
                                       components_.kinematic_body, components_.collider,
                                       components_.trigger};
    for (const ComponentTypeId component : writes) {
        if (Status declared = desc.access.write(component); !declared) {
            return make_unexpected(declared.error());
        }
    }
    const ComponentTypeId reads[4] = {scene_.world_transform, components_.material,
                                      components_.joint, components_.character_body};
    for (const ComponentTypeId component : reads) {
        if (Status declared = desc.access.read(component); !declared) {
            return make_unexpected(declared.error());
        }
    }
    return schedule.add(ecs::Stage::Physics, desc);
}

// --- Publication ---------------------------------------------------------------------------------

void PhysicsBridge::publish(BodyHandle body, UserData user_data, const Transform& transform,
                            bool teleported) noexcept {
    (void)body;
    (void)teleported;
    const ecs::Entity entity = entity_of(user_data);
    if (!world_->is_alive(entity)) {
        ++statistics_.orphan_publications;
        return;
    }
    auto* local = world_->get_mut<scene::LocalTransform>(entity, scene_.local_transform);
    if (local == nullptr) {
        // A body on an entity that is not placed in the scene. Legal — a pure-physics proxy has no
        // node — and nothing to publish to.
        return;
    }

    // THE SOLVER'S ANSWER IS IN WORLD SPACE AND `LocalTransform` IS NOT. For a root it is the same
    // value; for a child it is the parent's frame undone, which is the only arithmetic in this file
    // and the reason a parented body does not fly off when its parent is anywhere but the origin.
    Transform placement = transform;
    const ecs::Entity parent = world_->parent_of(entity);
    if (parent.valid()) {
        if (const auto* above = world_->get<scene::WorldTransform>(parent, scene_.world_transform);
            above != nullptr) {
            placement = inverse(above->value) * transform;
        }
    }
    // The solver has no opinion about scale — `physics` shapes carry their own dimensions — so the
    // authored scale is preserved rather than overwritten with the identity a body reports.
    placement.scale = local->value.scale;
    local->value = placement;
    ++statistics_.transforms_written;

    if (scene::mark_transform_changed(*tree_, entity)) {
        ++statistics_.transforms_marked;
    }
}

BodyHandle PhysicsBridge::body_of(ecs::Entity entity) const noexcept {
    const u32* slot = index_.find(entity.bits());
    return slot == nullptr ? BodyHandle() : tracked_[*slot].body;
}

// --- Teardown ------------------------------------------------------------------------------------

void PhysicsBridge::teardown() noexcept {
    if (torn_down_) {
        return;
    }
    torn_down_ = true;
    clock_ = nullptr;

    // BODIES BEFORE SHAPES. A shape destroyed while a body still references it is a use-after-free
    // in the backend, and the reference backend and Jolt would report it differently — or not at
    // all. This order is the one `samples/04-character::shutdown` takes with its character
    // controller, for the same reason it gives: "the controller owns a shape and a body in the
    // world, so it goes before the server does".
    for (const Tracked& tracked : tracked_) {
        if (!tracked.body.is_null()) {
            stepper_.untrack(tracked.body);
            (void)server_->destroy_body(tracked.body);
            ++statistics_.bodies_destroyed;
        }
    }
    tracked_.clear();
    index_.clear();

    for (const ShapeHandle shape : shapes_) {
        (void)server_->destroy_shape(shape);
    }
    shapes_.clear();
    pending_.clear();
    doomed_.clear();
    colliders_.clear();
}

}  // namespace cy::physics
