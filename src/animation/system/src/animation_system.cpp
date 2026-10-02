// SPDX-License-Identifier: MIT
// The per-frame animation system. See cy/animation/animation_system.h.

#include <cy/animation/animation_system.h>

#include <cy/core/jobs/parallel.h>
#include <cy/core/jobs/scratch.h>
#include <cy/core/math/scalar.h>
#include <cy/ecs/query.h>
#include <cy/scene/components.h>
#include <cy/scene/propagation.h>
#include <cy/scene/tree.h>

#include <atomic>

namespace cy::animation {
namespace {

/// A tier with an evaluation rate is due when its accumulated simulation time reaches the period.
/// The tolerance absorbs the rounding of summing 1/60 twice against 1/30, which would otherwise
/// make a 30 Hz instance on a 60 Hz clock miss every other evaluation by a few ulps.
constexpr f32 kDueTolerance = 1.0e-4F;

/// The calling thread's bit in `AnimationSystemStats::workers` when it is not a job worker.
constexpr u32 kCallerBit = 63;

[[nodiscard]] RootDelta compose(const RootDelta& first, const RootDelta& then) noexcept {
    RootDelta out;
    out.translation = first.translation + (first.rotation * then.translation);
    out.rotation = normalize(first.rotation * then.rotation);
    out.distance = first.distance + then.distance;
    out.contacts = then.contacts;
    return out;
}

[[nodiscard]] usize slice_transforms(const AnimationRig& rig) noexcept {
    return PoseScratch::transforms_needed(rig) +
           (static_cast<usize>(rig.skeleton().joint_count()) * 2U);
}

}  // namespace

const char* root_motion_mode_name(RootMotionMode mode) noexcept {
    switch (mode) {
        case RootMotionMode::Ignore:
            return "ignore";
        case RootMotionMode::ApplyToTransform:
            return "apply-to-transform";
        case RootMotionMode::Controller:
            return "controller";
        case RootMotionMode::ExtractOnly:
            return "extract-only";
    }
    return "unknown";
}

Expected<ecs::ComponentTypeId, Error> register_animator(ecs::World& world) noexcept {
    return world.components().register_builtin(kAnimatorComponentName,
                                               static_cast<u32>(sizeof(Animator)),
                                               static_cast<u32>(alignof(Animator)));
}

AnimationSystem::Batch::Batch(Allocator& allocator, const AnimationRig& rig) noexcept
    : instances(allocator, rig), owners(allocator) {}

/// What the workers share during one evaluation: the system, and the first failure.
struct AnimationSystem::EvaluateJob {
    AnimationSystem* system = nullptr;
    std::atomic<bool> failed{false};
    Error error;
    std::atomic<u64> workers{0};
};

AnimationSystem::AnimationSystem(Allocator& allocator, ecs::World& world,
                                 ecs::ComponentTypeId animator, scene::SceneTree* tree,
                                 const AnimationSystemConfig& config) noexcept
    : allocator_(&allocator),
      world_(&world),
      tree_(tree),
      animator_(animator),
      config_(config),
      batches_(allocator),
      slots_(allocator),
      free_slots_(allocator),
      index_(allocator),
      slices_(allocator),
      serial_scratch_(allocator),
      poses_(allocator),
      events_(allocator) {
    if (config_.slice == 0) {
        config_.slice = 1;
    }
}

AnimationSystem::~AnimationSystem() {
    teardown();
}

Expected<RigId, Error> AnimationSystem::add_rig(const AnimationRig& rig) noexcept {
    if (!rig.bound()) {
        return fail(ErrorCode::InvalidArgument,
                    "a rig is bound to its skeleton, program and clips before it is registered");
    }
    Expected<Batch*, Error> added = batches_.emplace_back(*allocator_, rig);
    if (!added) {
        return make_unexpected(added.error());
    }
    return static_cast<RigId>(batches_.size() - 1);
}

// --- Lookup --------------------------------------------------------------------------------------

const AnimationSystem::Slot* AnimationSystem::slot_of(ecs::Entity entity) const noexcept {
    const u32* found = index_.find(entity.bits());
    if (found == nullptr || *found >= slots_.size() || !slots_[*found].live) {
        return nullptr;
    }
    return &slots_[*found];
}

AnimationSystem::Slot* AnimationSystem::slot_of(ecs::Entity entity) noexcept {
    const u32* found = index_.find(entity.bits());
    if (found == nullptr || *found >= slots_.size() || !slots_[*found].live) {
        return nullptr;
    }
    return &slots_[*found];
}

bool AnimationSystem::tracks(const Animator& animator, ecs::Entity entity) const noexcept {
    const AnimatorHandle handle = animator.instance;
    if (!handle.valid() || handle.index >= slots_.size()) {
        return false;
    }
    const Slot& slot = slots_[handle.index];
    // The entity is compared as well as the generation: an `Animator` copied from another entity —
    // a duplicated prefab — carries a handle that is live and is not this entity's.
    return slot.live && slot.generation == handle.generation && slot.entity == entity &&
           slot.rig == animator.rig;
}

AnimationInstance* AnimationSystem::instance(ecs::Entity entity) noexcept {
    Slot* slot = slot_of(entity);
    return slot == nullptr ? nullptr : &batches_[slot->rig].instances.instance(slot->index);
}

const AnimationInstance* AnimationSystem::instance(ecs::Entity entity) const noexcept {
    const Slot* slot = slot_of(entity);
    if (slot == nullptr) {
        return nullptr;
    }
    return &batches_[slot->rig].instances.instance(slot->index);
}

const AnimationRig* AnimationSystem::rig_of(ecs::Entity entity) const noexcept {
    const Slot* slot = slot_of(entity);
    return slot == nullptr ? nullptr : &batches_[slot->rig].instances.rig();
}

Status AnimationSystem::set_parameter(ecs::Entity entity, Name parameter, f32 value) noexcept {
    const Slot* slot = slot_of(entity);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "this entity has no animation instance");
    }
    Batch& batch = batches_[slot->rig];
    return batch.instances.instance(slot->index)
        .set_parameter(batch.instances.rig(), parameter, value);
}

Expected<f32, Error> AnimationSystem::parameter(ecs::Entity entity, Name parameter) const noexcept {
    const Slot* slot = slot_of(entity);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "this entity has no animation instance");
    }
    const AnimationRig& rig = batches_[slot->rig].instances.rig();
    const Span<const Name> names = rig.program().parameters();
    for (const Name& name : names) {
        if (name == parameter) {
            return instance(entity)->parameter(rig, parameter);
        }
    }
    return fail(ErrorCode::NotFound, "the program declares no such parameter");
}

RootDelta AnimationSystem::root_motion(ecs::Entity entity) const noexcept {
    const AnimationInstance* found = instance(entity);
    return found == nullptr ? RootDelta{} : found->root_motion();
}

Vec3 AnimationSystem::travelled(ecs::Entity entity) const noexcept {
    const AnimationInstance* found = instance(entity);
    return found == nullptr ? Vec3{0.0F, 0.0F, 0.0F} : found->travelled();
}

RootDelta AnimationSystem::take_root_motion(ecs::Entity entity) noexcept {
    Slot* slot = slot_of(entity);
    if (slot == nullptr) {
        return RootDelta{};
    }
    const RootDelta taken = slot->pending;
    slot->pending = RootDelta{};
    return taken;
}

PoseHandle AnimationSystem::pose_of(ecs::Entity entity) const noexcept {
    const Slot* slot = slot_of(entity);
    return slot == nullptr ? PoseHandle{} : slot->pose;
}

ecs::Entity AnimationSystem::entity_of(const EmittedEvent& event) const noexcept {
    if (event.instance >= slots_.size() || !slots_[event.instance].live) {
        return ecs::kNoEntity;
    }
    return slots_[event.instance].entity;
}

void AnimationSystem::refuse(const char* reason) noexcept {
    ++stats_.refused;
    last_error_ = fail(ErrorCode::InvalidArgument, reason);
}

// --- Sync ----------------------------------------------------------------------------------------

void AnimationSystem::configure(Slot& slot, const Animator& animator) noexcept {
    AnimationInstance& instance = batches_[slot.rig].instances.instance(slot.index);
    instance.set_tier(animator.tier);
    instance.set_event_policy(animator.events);
    instance.set_play_rate(animator.play_rate);
    slot.mode = animator.root_motion;
    if (slot.mode == RootMotionMode::ApplyToTransform && tree_ == nullptr) {
        refuse(
            "an Animator applies root motion to its transform, and this system was given no "
            "scene tree to write it through");
        slot.mode = RootMotionMode::Ignore;
    }
}

Status AnimationSystem::publish_reference(Slot& slot) noexcept {
    const Skeleton& skeleton = batches_[slot.rig].instances.rig().skeleton();
    const usize joints = skeleton.joint_count();
    if (serial_scratch_.size() < joints * 2U) {
        if (Status sized = serial_scratch_.resize(joints * 2U); !sized) {
            return sized;
        }
    }
    const Span<Transform> local = serial_scratch_.span().subspan(0, joints);
    const Span<Transform> model = serial_scratch_.span().subspan(joints, joints);
    skeleton.reference_pose(local);
    const JointMask& all = skeleton.retained(0);
    skeleton.to_model(local, all, model);
    skeleton.to_skinning(model, all, poses_.staging(slot.pose));
    return poses_.commit(slot.pose);
}

Status AnimationSystem::create(ecs::Entity entity) noexcept {
    const auto* animator = world_->get<Animator>(entity, animator_);
    if (animator == nullptr) {
        return ok();
    }
    if (animator->rig >= batches_.size()) {
        refuse("an Animator names a rig this animation system was not given");
        return ok();
    }
    u32 index = 0;
    if (!free_slots_.empty()) {
        index = free_slots_.back();
        free_slots_.pop_back();
    } else {
        if (Status pushed = slots_.push_back(Slot{}); !pushed) {
            return pushed;
        }
        index = static_cast<u32>(slots_.size() - 1);
    }
    Batch& batch = batches_[animator->rig];
    Expected<u32, Error> added = batch.instances.add();
    if (!added) {
        (void)free_slots_.push_back(index);
        return Status{make_unexpected(added.error())};
    }
    if (Status owned = batch.owners.push_back(index); !owned) {
        (void)batch.instances.remove(*added);
        (void)free_slots_.push_back(index);
        return owned;
    }
    Expected<PoseHandle, Error> pose = poses_.add(batch.instances.rig().skeleton().joint_count());
    if (!pose) {
        (void)batch.instances.remove(*added);
        batch.owners.pop_back();
        (void)free_slots_.push_back(index);
        return Status{make_unexpected(pose.error())};
    }

    Slot& slot = slots_[index];
    slot.entity = entity;
    slot.rig = animator->rig;
    slot.index = *added;
    slot.pose = *pose;
    slot.since_evaluated = 0.0F;
    slot.pending = RootDelta{};
    slot.live = true;
    // Evaluated on its first frame whatever its tier's rate, so the pose world never shows an
    // instance it has not posed. A `Baked` instance keeps the reference pose published here.
    slot.due = true;
    // The event identifier is the SLOT, which a swap-remove in the batch does not move.
    batch.instances.instance(slot.index).set_identifier(index);
    configure(slot, *animator);
    if (Status published = publish_reference(slot); !published) {
        return published;
    }
    if (Expected<u32*, Error> inserted = index_.insert(entity.bits(), index); !inserted) {
        return Status{make_unexpected(inserted.error())};
    }

    if (auto* writable = world_->get_mut<Animator>(entity, animator_); writable != nullptr) {
        writable->instance = AnimatorHandle{index, slot.generation};
        writable->pose = slot.pose;
    }
    ++stats_.instances_added;
    return ok();
}

Status AnimationSystem::destroy(u32 index) noexcept {
    Slot& slot = slots_[index];
    Batch& batch = batches_[slot.rig];
    const u32 last = batch.instances.size() - 1;
    if (Status removed = batch.instances.remove(slot.index); !removed) {
        return removed;
    }
    if (slot.index != last) {
        // THE ONE ENTRY THAT MOVED. The batch packed itself by moving its last instance into the
        // hole; its owning slot is the only one whose index changes.
        const u32 moved = batch.owners[last];
        batch.owners[slot.index] = moved;
        slots_[moved].index = slot.index;
    }
    batch.owners.pop_back();
    (void)poses_.remove(slot.pose);

    const u32* mapped = index_.find(slot.entity.bits());
    if (mapped != nullptr && *mapped == index) {
        (void)index_.remove(slot.entity.bits());
    }
    if (world_->is_alive(slot.entity)) {
        if (auto* writable = world_->get_mut<Animator>(slot.entity, animator_);
            writable != nullptr && writable->instance == AnimatorHandle{index, slot.generation}) {
            writable->instance = AnimatorHandle{};
            writable->pose = PoseHandle{};
        }
    }
    slot.live = false;
    slot.due = false;
    slot.entity = ecs::kNoEntity;
    ++slot.generation;
    ++stats_.instances_removed;
    return free_slots_.push_back(index);
}

Status AnimationSystem::sync() noexcept {
    // THE WALK COLLECTS AND DOES NOT ACT. Creating an instance writes its handle back into the
    // component the query is reading, and a write into a chunk under iteration is the shape of
    // defect `PhysicsBridge::sync` documents.
    Array<ecs::Entity> pending(*allocator_);
    ecs::QueryDesc desc(world_->allocator());
    if (Status declared = desc.read(animator_); !declared) {
        return declared;
    }
    ecs::Query query(*world_, std::move(desc));
    Status collected = ok();
    Status walked = query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        const Span<const ecs::Entity> entities = chunk.entities();
        const Span<const Animator> animators = chunk.read<Animator>(animator_);
        for (usize row = 0; row < entities.size() && collected; ++row) {
            if (tracks(animators[row], entities[row])) {
                configure(slots_[animators[row].instance.index], animators[row]);
            } else {
                collected = pending.push_back(entities[row]);
            }
        }
    });
    if (!walked) {
        return walked;
    }
    if (!collected) {
        return collected;
    }

    // Removal in slot order: an instance whose entity died, lost its `Animator`, or carries one
    // that no longer names this instance (a changed rig, a replaced component).
    for (u32 index = 0; index < slots_.size(); ++index) {
        const Slot& slot = slots_[index];
        if (!slot.live) {
            continue;
        }
        const Animator* animator =
            world_->is_alive(slot.entity) ? world_->get<Animator>(slot.entity, animator_) : nullptr;
        if (animator == nullptr || !tracks(*animator, slot.entity)) {
            if (Status removed = destroy(index); !removed) {
                return removed;
            }
        }
    }
    for (const ecs::Entity entity : pending) {
        if (Status created = create(entity); !created) {
            return created;
        }
    }
    stats_.instances = static_cast<u32>(index_.size());
    stats_.batches = 0;
    for (const Batch& batch : batches_) {
        stats_.batches += batch.instances.size() != 0 ? 1U : 0U;
    }
    return ok();
}

// --- Advance -------------------------------------------------------------------------------------

Status AnimationSystem::consume_root_motion(f32 tick_seconds) noexcept {
    (void)tick_seconds;
    for (Slot& slot : slots_) {
        if (!slot.live || slot.mode == RootMotionMode::Ignore ||
            slot.mode == RootMotionMode::ExtractOnly) {
            continue;
        }
        const RootDelta& delta = batches_[slot.rig].instances.instance(slot.index).root_motion();
        if (slot.mode == RootMotionMode::Controller) {
            slot.pending = compose(slot.pending, delta);
            continue;
        }
        // ApplyToTransform: the delta is in the character's own frame, so it is rotated by the
        // entity's placement before it moves it.
        auto* local = world_->get_mut<scene::LocalTransform>(slot.entity,
                                                             tree_->components().local_transform);
        if (local == nullptr) {
            continue;
        }
        local->value.translation =
            local->value.translation + (local->value.rotation * delta.translation);
        local->value.rotation = normalize(local->value.rotation * delta.rotation);
        if (Status marked = scene::mark_transform_changed(*tree_, slot.entity); !marked) {
            return marked;
        }
    }
    return ok();
}

void AnimationSystem::schedule_evaluation(f32 seconds) noexcept {
    for (Slot& slot : slots_) {
        if (!slot.live) {
            continue;
        }
        const LodTier tier = batches_[slot.rig].instances.instance(slot.index).tier();
        if (!evaluates_pose(tier)) {
            continue;
        }
        const f32 hertz = evaluation_hertz(tier);
        if (hertz <= 0.0F) {
            slot.due = true;
            continue;
        }
        const f32 period = 1.0F / hertz;
        slot.since_evaluated += seconds;
        if (slot.since_evaluated + kDueTolerance >= period) {
            slot.due = true;
            slot.since_evaluated -= period;
            // A long stall is not owed a burst of evaluations; one is enough to catch up the pose.
            if (slot.since_evaluated >= period) {
                slot.since_evaluated = 0.0F;
            }
        }
    }
}

Status AnimationSystem::advance(u32 ticks, f32 tick_seconds) noexcept {
    events_.clear();
    return step(ticks, tick_seconds);
}

Status AnimationSystem::step(u32 ticks, f32 tick_seconds) noexcept {
    stats_.ticks = ticks;
    for (u32 tick = 0; tick < ticks; ++tick) {
        for (Batch& batch : batches_) {
            if (Status advanced = batch.instances.advance_all(tick_seconds, &events_); !advanced) {
                return advanced;
            }
        }
        if (Status consumed = consume_root_motion(tick_seconds); !consumed) {
            return consumed;
        }
        schedule_evaluation(tick_seconds);
    }
    stats_.ticks_total += ticks;
    return ok();
}

// --- Evaluate ------------------------------------------------------------------------------------

Status AnimationSystem::evaluate_slice(const Slice& slice, Span<Transform> scratch) noexcept {
    Batch& batch = batches_[slice.rig];
    const AnimationRig& rig = batch.instances.rig();
    const Skeleton& skeleton = rig.skeleton();
    const usize joints = skeleton.joint_count();
    const usize program = PoseScratch::transforms_needed(rig);

    // Constructed over an allocator it never uses: `adopt` hands it the slice's own buffer.
    PoseScratch pose_scratch(*allocator_);
    if (Status adopted = pose_scratch.adopt(rig, scratch.subspan(0, program)); !adopted) {
        return adopted;
    }
    const Span<Transform> local = scratch.subspan(program, joints);
    const Span<Transform> model = scratch.subspan(program + joints, joints);

    for (u32 offset = 0; offset < slice.count; ++offset) {
        const u32 index = slice.first + offset;
        const Slot& slot = slots_[batch.owners[index]];
        AnimationInstance& instance = batch.instances.instance(index);
        if (!slot.due || !evaluates_pose(instance.tier())) {
            continue;
        }
        const u8 bone_lod = bone_lod_for(instance.tier());
        skeleton.reference_pose(local);
        EvaluationStats stats;
        if (Status evaluated =
                animation::evaluate(rig, instance, bone_lod, pose_scratch, local, stats);
            !evaluated) {
            return evaluated;
        }
        // Straight into the pose world's staging half: the same arithmetic `publish_pose` does,
        // without a matrix scratch to copy out of.
        const JointMask& retained = skeleton.retained(bone_lod);
        skeleton.to_model(local, retained, model);
        skeleton.to_skinning(model, retained, poses_.staging(slot.pose));
    }
    return ok();
}

Status AnimationSystem::evaluate(jobs::JobSystem* jobs) noexcept {
    slices_.clear();
    usize widest = 0;
    for (u32 rig = 0; rig < batches_.size(); ++rig) {
        const Batch& batch = batches_[rig];
        const u32 size = batch.instances.size();
        for (u32 first = 0; first < size; first += config_.slice) {
            const u32 count = (size - first) < config_.slice ? (size - first) : config_.slice;
            if (Status pushed = slices_.push_back(Slice{rig, first, count}); !pushed) {
                return pushed;
            }
        }
        if (size != 0) {
            const usize needed = slice_transforms(batch.instances.rig());
            widest = needed > widest ? needed : widest;
        }
    }
    stats_.slices = static_cast<u32>(slices_.size());

    if (jobs == nullptr) {
        if (serial_scratch_.size() < widest) {
            if (Status sized = serial_scratch_.resize(widest); !sized) {
                return sized;
            }
        }
        for (const Slice& slice : slices_) {
            if (Status ran = evaluate_slice(slice, serial_scratch_.span()); !ran) {
                return ran;
            }
        }
        stats_.workers |= u64{1} << kCallerBit;
    } else if (!slices_.empty()) {
        EvaluateJob job;
        job.system = this;
        auto body = [&job](const jobs::TaskContext& context, u64 begin, u64 end) noexcept {
            const u32 bit =
                context.worker < kCallerBit ? static_cast<u32>(context.worker) : kCallerBit;
            job.workers.fetch_or(u64{1} << bit, std::memory_order_relaxed);
            for (u64 item = begin; item < end; ++item) {
                const Slice& slice = job.system->slices_[item];
                const usize needed =
                    slice_transforms(job.system->batches_[slice.rig].instances.rig());
                Status ran = ok();
                // THE WORKER'S SCRATCH ARENA, released when the slice ends. A rig too large for it
                // falls back to the heap rather than failing a frame.
                Transform* storage = nullptr;
                usize mark = 0;
                if (context.scratch != nullptr) {
                    mark = context.scratch->mark();
                    storage = context.scratch->allocate_array<Transform>(needed);
                }
                if (storage != nullptr) {
                    ran = job.system->evaluate_slice(slice, Span<Transform>(storage, needed));
                    context.scratch->release_to(mark);
                } else {
                    Array<Transform> heap(*job.system->allocator_);
                    ran = heap.resize(needed);
                    if (ran) {
                        ran = job.system->evaluate_slice(slice, heap.span());
                    }
                }
                bool expected = false;
                if (!ran && job.failed.compare_exchange_strong(expected, true)) {
                    job.error = ran.error();
                }
            }
        };
        if (Status ran = jobs::parallel_for(*jobs, slices_.size(), 1, body,
                                            "cy::animation::AnimationSystem::evaluate");
            !ran) {
            return ran;
        }
        if (job.failed.load()) {
            return Status{make_unexpected(job.error)};
        }
        stats_.workers |= job.workers.load();
    }

    // THE COMMITS, in slot order on this thread: `PoseWorld::commit` is not thread-safe, and its
    // dirty range is the renderer's upload range.
    stats_.evaluated = 0;
    stats_.skipped = 0;
    for (Slot& slot : slots_) {
        if (!slot.live) {
            continue;
        }
        const LodTier tier = batches_[slot.rig].instances.instance(slot.index).tier();
        if (!slot.due || !evaluates_pose(tier)) {
            slot.due = false;
            ++stats_.skipped;
            continue;
        }
        slot.due = false;
        if (Status committed = poses_.commit(slot.pose); !committed) {
            return committed;
        }
        ++stats_.evaluated;
    }
    return ok();
}

Status AnimationSystem::run(u32 ticks, f32 tick_seconds, jobs::JobSystem* jobs) noexcept {
    if (Status synced = sync(); !synced) {
        return synced;
    }
    if (Status advanced = advance(ticks, tick_seconds); !advanced) {
        return advanced;
    }
    return evaluate(jobs);
}

Expected<ecs::SystemId, Error> AnimationSystem::install(
    ecs::Schedule& schedule, const determinism::SimulationClock& clock) noexcept {
    clock_ = &clock;

    // WHAT BOTH HALVES TOUCH. `Animator` is WRITTEN by both — `sync` writes the instance and pose
    // handles back into it. With a scene tree, the tick half also writes `LocalTransform` and sets
    // `NodeState`'s dirty bits for the instances whose root motion moves their transform.
    ecs::SystemDesc tick;
    tick.name = "cy::animation::AnimationSystem::advance";
    tick.user = this;
    tick.body = [](const ecs::SystemContext& context) noexcept {
        auto* system = static_cast<AnimationSystem*>(context.user);
        if (system == nullptr || system->clock_ == nullptr) {
            return;
        }
        // The first tick after a frame starts a new set of events and a new report.
        if (system->events_read_) {
            system->events_.clear();
            system->last_error_ = ok();
            system->events_read_ = false;
        }
        Status ran = system->sync();
        if (ran) {
            ran = system->step(1, system->clock_->delta_seconds());
        }
        if (!ran) {
            system->last_error_ = ran;
        }
    };
    if (Status declared = tick.access.write(animator_); !declared) {
        return make_unexpected(declared.error());
    }
    if (tree_ != nullptr) {
        if (Status declared = tick.access.write(tree_->components().local_transform); !declared) {
            return make_unexpected(declared.error());
        }
        if (Status declared = tick.access.write(tree_->components().state); !declared) {
            return make_unexpected(declared.error());
        }
    }
    Expected<ecs::SystemId, Error> ticked = schedule.add(ecs::Stage::PostSimulation, tick);
    if (!ticked) {
        return ticked;
    }
    tick_system_ = *ticked;

    ecs::SystemDesc frame;
    frame.name = "cy::animation::AnimationSystem::evaluate";
    frame.user = this;
    frame.body = [](const ecs::SystemContext& context) noexcept {
        auto* system = static_cast<AnimationSystem*>(context.user);
        if (system == nullptr) {
            return;
        }
        jobs::JobSystem* workers = context.jobs != nullptr && context.jobs->task != nullptr
                                       ? context.jobs->task->system
                                       : nullptr;
        Status ran = system->sync();
        if (ran) {
            ran = system->evaluate(workers);
        }
        system->events_read_ = true;
        if (!ran) {
            system->last_error_ = ran;
        }
    };
    if (Status declared = frame.access.write(animator_); !declared) {
        return make_unexpected(declared.error());
    }
    return schedule.add(ecs::Stage::Animation, frame);
}

void AnimationSystem::teardown() noexcept {
    for (u32 index = 0; index < slots_.size(); ++index) {
        if (slots_[index].live) {
            (void)destroy(index);
        }
    }
    batches_.clear();
    slots_.clear();
    free_slots_.clear();
    index_.clear();
    slices_.clear();
    events_.clear();
}

}  // namespace cy::animation
