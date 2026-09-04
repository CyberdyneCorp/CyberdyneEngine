// Stages, and the ECS's binding onto M1's system schedule. Task 2.5.

#include <cy/ecs/system.h>

#include <cy/core/jobs/types.h>

namespace cy::ecs {
namespace {

/// Lexicographic comparison of two system names, by content. A local loop rather than std::strcmp
/// because that is all it needs to be, and a null name cannot reach here — `SystemSchedule::add`
/// refuses a system without one, and refuses a duplicate, so the names of one stage are a set and
/// this is a total order over them.
[[nodiscard]] bool name_less(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return right != nullptr;
    }
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return static_cast<unsigned char>(*left) < static_cast<unsigned char>(*right);
}

}  // namespace

const char* stage_name(Stage stage) noexcept {
    switch (stage) {
        case Stage::PreSimulation:
            return "PreSimulation";
        case Stage::Physics:
            return "Physics";
        case Stage::Simulation:
            return "Simulation";
        case Stage::PostSimulation:
            return "PostSimulation";
        case Stage::Frame:
            return "Frame";
        case Stage::Animation:
            return "Animation";
        case Stage::UI:
            return "UI";
        case Stage::Render:
            return "Render";
    }
    return "unknown";
}

Schedule::~Schedule() {
    Allocator& allocator = world_->allocator();
    for (StageState* state : stages_) {
        if (state == nullptr) {
            continue;
        }
        for (Registration* registration : state->systems) {
            registration->~Registration();
            allocator.deallocate(static_cast<void*>(registration), sizeof(Registration),
                                 alignof(Registration));
        }
        state->~StageState();
        allocator.deallocate(static_cast<void*>(state), sizeof(StageState), alignof(StageState));
    }
}

Expected<Schedule::StageState*, Error> Schedule::stage_state(Stage stage) noexcept {
    const auto index = static_cast<u32>(stage);
    if (stages_[index] != nullptr) {
        return stages_[index];
    }
    // A stage's schedule is tens of kilobytes — `jobs::SystemSchedule` holds 128 systems and their
    // edge matrix inline — so the eight of them are allocated on demand rather than being members.
    // A world with three systems in one stage pays for one.
    void* block = world_->allocator().allocate(sizeof(StageState), alignof(StageState));
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not allocate a stage schedule");
    }
    stages_[index] = ::new (block) StageState(world_->allocator());
    return stages_[index];
}

void Schedule::trampoline(const jobs::SystemContext& context, void* user) noexcept {
    auto* registration = static_cast<Registration*>(user);
    SystemContext ecs_context;
    ecs_context.world = registration->owner->world_;
    ecs_context.commands = &registration->commands;
    ecs_context.jobs = &context;
    ecs_context.user = registration->user;
    ecs_context.stage = registration->stage;
    ecs_context.system = registration->system;

    const i64 started = jobs::monotonic_now_ns();
    registration->body(ecs_context);
    const i64 elapsed = jobs::monotonic_now_ns() - started;

    // Measured in every configuration, not only where assertions survive: a per-system cost that
    // exists only in a Debug build is a number nobody profiles against.
    registration->profile.last_ns = static_cast<u64>((elapsed < 0) ? 0 : elapsed);
    registration->profile.total_ns += registration->profile.last_ns;
    ++registration->profile.runs;
    registration->profile.commands_recorded += registration->commands.pending();
}

Expected<SystemId, Error> Schedule::add(Stage stage, const SystemDesc& desc) noexcept {
    if (desc.name == nullptr || desc.body == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a system needs a name and a body");
    }
    Expected<StageState*, Error> state = stage_state(stage);
    if (!state) {
        return make_unexpected(state.error());
    }

    Allocator& allocator = world_->allocator();
    void* block = allocator.allocate(sizeof(Registration), alignof(Registration));
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not allocate a system registration");
    }
    // A provisional merge key: the registration index, which is a total order and nothing more.
    // The real one is assigned by `build()`, which is the first moment every system in the stage
    // has a name to be ranked by — see assign_merge_keys() and command_buffer.h. A schedule that is
    // run without being built is refused, so the provisional key never decides a flush.
    auto* registration =
        ::new (block) Registration(*world_, static_cast<u32>((*state)->systems.size()));
    registration->owner = this;
    registration->body = desc.body;
    registration->user = desc.user;
    registration->stage = stage;
    registration->profile.name = desc.name;
    registration->profile.stage = stage;

    const auto destroy_registration = [&]() noexcept {
        registration->~Registration();
        allocator.deallocate(block, sizeof(Registration), alignof(Registration));
    };

    jobs::SystemDesc jobs_desc;
    jobs_desc.name = desc.name;
    jobs_desc.body = &Schedule::trampoline;
    jobs_desc.user = registration;
    jobs_desc.access = desc.access;
    jobs_desc.after = desc.after.empty() ? nullptr : desc.after.data();
    jobs_desc.after_count = static_cast<u32>(desc.after.size());

    Expected<SystemId, Error> id = (*state)->schedule.add(jobs_desc);
    if (!id) {
        destroy_registration();
        return id;
    }
    registration->system = *id;

    if (Status pushed = (*state)->systems.push_back(registration); !pushed) {
        destroy_registration();
        return make_unexpected(pushed.error());
    }
    if (Status attached = world_->attach(registration->commands); !attached) {
        return make_unexpected(attached.error());
    }
    (*state)->built = false;
    return *id;
}

Status Schedule::order(Stage stage, SystemId before, SystemId after) noexcept {
    StageState* state = stages_[static_cast<u32>(stage)];
    if (state == nullptr) {
        return fail(ErrorCode::NotFound, "that stage has no systems");
    }
    if (Status ordered = state->schedule.order(before, after); !ordered) {
        return ordered;
    }
    state->built = false;
    return ok();
}

Status Schedule::build() noexcept {
    for (StageState* state : stages_) {
        if (state == nullptr || state->built) {
            continue;
        }
        if (Status built = state->schedule.build(); !built) {
            return built;
        }
        if (Status keyed = assign_merge_keys(*state); !keyed) {
            return keyed;
        }
        state->built = true;
    }
    return ok();
}

Status Schedule::assign_merge_keys(StageState& state) noexcept {
    // Task 1.7. The command buffers of one stage flush in (system, thread, record) order, and
    // `system` is this rank: the system's position among its stage's systems **in name order**.
    // Registration order was what it used to be, and registration order is a sequence number
    // standing in for an identity — the moment a plugin or a game mode registers a system
    // conditionally, every later system's key shifts and two conflicting spawns swap places in the
    // flush. `StateProviderRegistry::finalize()` is the shape this follows, and
    // `simulation-and-determinism` is the requirement: a registry whose contents affect simulation
    // is finalised in an order derived from stable identifiers.
    //
    // Insertion sort over an index array. Allocation-free, stable, and over a list that
    // `jobs::SystemSchedule` caps at 128 — sorting it is nothing beside building the stage's
    // dependency graph, which is quadratic in the same number.
    const u32 count = static_cast<u32>(state.systems.size());
    u32 order[jobs::SystemSchedule::kMaxSystems];
    for (u32 i = 0; i < count; ++i) {
        order[i] = i;
    }
    for (u32 i = 1; i < count; ++i) {
        for (u32 j = i; j > 0 && name_less(state.systems[order[j]]->profile.name,
                                           state.systems[order[j - 1]]->profile.name);
             --j) {
            const u32 held = order[j - 1];
            order[j - 1] = order[j];
            order[j] = held;
        }
    }
    for (u32 rank = 0; rank < count; ++rank) {
        // Refused only when the buffer still holds commands, which would mean a stage was rebuilt
        // between a system recording and the flush that applies it. That is a defect in the caller
        // and it is reported rather than absorbed: the alternative is a command applied at a point
        // in the merge that neither key describes.
        if (Status keyed = state.systems[order[rank]]->commands.set_merge_key(rank, 0); !keyed) {
            return keyed;
        }
    }
    return ok();
}

Status Schedule::finish_stage(StageState& state) noexcept {
    // The stage's flush point: every command buffer is applied here, in (system, thread, record)
    // order, and nothing structural has happened before it.
    (void)state;
    Expected<u64, Error> applied = world_->flush();
    if (!applied) {
        return make_unexpected(applied.error());
    }
    return ok();
}

Status Schedule::run(Stage stage, jobs::JobSystem& jobs) noexcept {
    StageState* state = stages_[static_cast<u32>(stage)];
    if (state == nullptr) {
        return ok();
    }
    if (!state->built) {
        return fail(ErrorCode::Unavailable, "the schedule has not been built since the last add()");
    }
    // One version per stage: everything written during it is stamped with the same number, which is
    // what makes "changed since the last time this system ran" a single comparison.
    world_->advance_version();
    if (Status ran = state->schedule.run(jobs); !ran) {
        return ran;
    }
    return finish_stage(*state);
}

Status Schedule::run_serial(Stage stage) noexcept {
    StageState* state = stages_[static_cast<u32>(stage)];
    if (state == nullptr) {
        return ok();
    }
    if (!state->built) {
        return fail(ErrorCode::Unavailable, "the schedule has not been built since the last add()");
    }
    world_->advance_version();

    // Batch by batch, and within a batch in the order the levelling put them. Parallelism is
    // derived rather than declared, so running the same plan on one thread produces the same
    // result — which is what makes this a legitimate way to run a schedule rather than a mock.
    for (u32 batch = 0; batch < state->schedule.batch_count(); ++batch) {
        for (u32 index = 0; index < state->schedule.batch_size(batch); ++index) {
            const SystemId system = state->schedule.batch_member(batch, index);
            auto* registration = static_cast<Registration*>(state->schedule.user_of(system));
            jobs::SystemContext context;
            context.system = system;
            context.access = &state->schedule.access_of(system);
            context.guard =
                jobs::SystemAccessGuard(state->schedule.name_of(system), *context.access);
            Schedule::trampoline(context, registration);
        }
    }
    return finish_stage(*state);
}

u32 Schedule::system_count(Stage stage) const noexcept {
    const StageState* state = stage_state(stage);
    return (state == nullptr) ? 0 : state->schedule.system_count();
}

u32 Schedule::batch_count(Stage stage) const noexcept {
    const StageState* state = stage_state(stage);
    return (state == nullptr) ? 0 : state->schedule.batch_count();
}

u32 Schedule::batch_size(Stage stage, u32 batch) const noexcept {
    const StageState* state = stage_state(stage);
    return (state == nullptr) ? 0 : state->schedule.batch_size(batch);
}

bool Schedule::ordered_before(Stage stage, SystemId before, SystemId after) const noexcept {
    const StageState* state = stage_state(stage);
    return state != nullptr && state->schedule.ordered_before(before, after);
}

bool Schedule::conflicts(Stage stage, SystemId first, SystemId second,
                         jobs::AccessConflict& out) const noexcept {
    const StageState* state = stage_state(stage);
    return state != nullptr && state->schedule.conflicts(first, second, out);
}

const SystemProfile* Schedule::profile(Stage stage, SystemId system) const noexcept {
    const StageState* state = stage_state(stage);
    if (state == nullptr || system >= state->systems.size()) {
        return nullptr;
    }
    return &state->systems[system]->profile;
}

CommandBuffer* Schedule::commands(Stage stage, SystemId system) noexcept {
    StageState* state = stages_[static_cast<u32>(stage)];
    if (state == nullptr || system >= state->systems.size()) {
        return nullptr;
    }
    return &state->systems[system]->commands;
}

Status Schedule::collect_profiles(Array<SystemProfile>& out) const noexcept {
    for (const StageState* state : stages_) {
        if (state == nullptr) {
            continue;
        }
        for (const Registration* registration : state->systems) {
            if (Status pushed = out.push_back(registration->profile); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

}  // namespace cy::ecs
