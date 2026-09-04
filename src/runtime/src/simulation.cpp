#include <cy/runtime/simulation.h>

#include <cy/core/base/assert.h>
#include <cy/core/base/diagnostic_sink.h>

namespace cy::runtime {
namespace {

using determinism::TickPhase;

/// The four stages of the fixed step and the four of the variable one, in `ecs-core`'s order.
/// Written out rather than derived from `stage_is_fixed_step()` so that the loop reads as the
/// specification's list — this is the pipeline, and a reader should see it, not compute it.
constexpr ecs::Stage kFixedStages[] = {ecs::Stage::PreSimulation, ecs::Stage::Physics,
                                       ecs::Stage::Simulation, ecs::Stage::PostSimulation};
constexpr ecs::Stage kFrameStages[] = {ecs::Stage::Frame, ecs::Stage::Animation, ecs::Stage::UI,
                                       ecs::Stage::Render};

static_assert((sizeof(kFixedStages) / sizeof(kFixedStages[0])) +
                      (sizeof(kFrameStages) / sizeof(kFrameStages[0])) ==
                  ecs::kStageCount,
              "every stage belongs to exactly one half of the frame");

}  // namespace

determinism::Participates Simulation::ClockProvider::participation() const noexcept {
    // Hashed, so that two runs at different ticks diverge at a named subsystem rather than deep in
    // the entity data. Checkpointed, because a restore that resumed at the wrong tick would be a
    // silently different session. Not part of a rollback capture: rollback moves the tick itself,
    // so the clock is what does the moving rather than something restored by it.
    return determinism::Participates::Hash | determinism::Participates::Checkpoint |
           determinism::Participates::Save;
}

Status Simulation::ClockProvider::hash(determinism::StateHashTree& tree) const noexcept {
    const determinism::SimulationPoint point = owner_->clock_.now();
    tree.mix_u64(point.epoch.value);
    tree.mix_u64(point.tick);
    tree.mix_u64(owner_->clock_.rate().numerator);
    tree.mix_u64(owner_->clock_.rate().denominator);
    return ok();
}

determinism::Participates Simulation::RandomProvider::participation() const noexcept {
    return determinism::Participates::Hash | determinism::Participates::Checkpoint |
           determinism::Participates::Save;
}

Status Simulation::RandomProvider::hash(determinism::StateHashTree& tree) const noexcept {
    // The seed and the mixer version, and nothing else — a counter-based stream has no state to
    // capture, which is the whole reason random.h has no mutable member. The mixer version is here
    // because a build that changed the mixing produces different draws from the same seed, and
    // discovering that as an entity-level divergence would waste a day.
    tree.mix_u64(owner_->random_.seed());
    tree.mix_u64(determinism::kMixerVersion);
    return ok();
}

Simulation::Simulation(Allocator& allocator, const SimulationConfig& config) noexcept
    : config_(config),
      world_(allocator, ecs::WorldConfig{config.world_name, config.chunk_bytes}),
      tree_(world_),
      schedule_(world_),
      commands_(allocator),
      commit_(allocator),
      providers_(allocator),
      schema_(allocator),
      hash_tree_(allocator) {}

Simulation::~Simulation() = default;

Nanoseconds Simulation::diagnostic_now() const noexcept {
    if (config_.diagnostic_clock == nullptr) {
        return 0;
    }
    return config_.diagnostic_clock(config_.diagnostic_clock_user);
}

Status Simulation::initialize() noexcept {
    CY_ASSERT_MSG(!initialized_, "Simulation::initialize() on a simulation already initialised");

    if (Status configured = clock_.configure(config_.clock); !configured) {
        return configured;
    }
    random_ = determinism::RandomSource(config_.session_seed);

    if (Status world_ready = world_.initialize(); !world_ready) {
        return world_ready;
    }

    if (config_.create_scene_tree) {
        if (Status tree_ready = tree_.initialize(); !tree_ready) {
            return tree_ready;
        }
        if (Status installed = tree_.install_systems(schedule_); !installed) {
            return installed;
        }
        tree_.set_time_step(clock_.delta_seconds(), clock_.delta_seconds());
        tree_created_ = true;
    }

    if (Status clock_registered = providers_.add(clock_provider_); !clock_registered) {
        return clock_registered;
    }
    if (Status random_registered = providers_.add(random_provider_); !random_registered) {
        return random_registered;
    }

    initialized_ = true;
    return ok();
}

Status Simulation::finalize_registration() noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "Simulation::initialize() has not run");
    }
    if (closed_) {
        return ok();
    }

    SchemaDeclarationReport declaration;
    if (Status declared = declare_reflected_components(world_, schema_, declaration); !declared) {
        return declared;
    }
    if (declaration.skipped_unreflected != 0) {
        // Said out loud rather than counted quietly. A world whose built-in components are not in
        // the hash has a hash that is silent about them, and the first person to be surprised by
        // that should be surprised at startup and not at a divergence report.
        emit_diagnosticf(DiagnosticSeverity::Info, "runtime",
                         "%u component types have no reflected descriptor and are not in the state "
                         "hash; declare them with StateSchema::declare() to cover them",
                         declaration.skipped_unreflected);
    }
    schema_.freeze();
    providers_.finalize();

    if (Status built = schedule_.build(); !built) {
        return built;
    }
    closed_ = true;
    return ok();
}

Status Simulation::run_stage(ecs::Stage stage, jobs::JobSystem* jobs) noexcept {
    if (schedule_.system_count(stage) == 0) {
        return ok();
    }
    return jobs != nullptr ? schedule_.run(stage, *jobs) : schedule_.run_serial(stage);
}

Status Simulation::take_hash() noexcept {
    // One tree, with the providers folded in as Subsystem nodes inside the world node — see
    // `hash_world`'s comment on why they are not a second tree.
    return hash_world(world_, schema_, hash_tree_, hash_report_, &providers_);
}

Expected<determinism::CommitRecord, Error> Simulation::step(jobs::JobSystem* jobs) noexcept {
    if (!closed_) {
        return fail(
            ErrorCode::Unavailable,
            "Simulation::finalize_registration() has not run; a registry that closed itself "
            "when the first tick arrived would close at a different point in a run that "
            "ticked earlier");
    }

    if (!tree_created_ && commands_.pending() != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "frame commands were recorded on a simulation with no scene tree; every kind "
                    "this queue carries acts on the tree, so there is nothing to apply them to");
    }

    tick_stats_ = TickStats{};
    const Nanoseconds tick_began = diagnostic_now();
    Nanoseconds phase_began = tick_began;
    const auto close_phase = [&](TickPhase phase) noexcept {
        const Nanoseconds now = diagnostic_now();
        tick_stats_.phase_ns[static_cast<u32>(phase)] = static_cast<u64>(now - phase_began);
        phase_began = now;
    };

    const u64 structural_before = world_.structural_changes();

    // --- IngestCommands ---------------------------------------------------------------------
    //
    // The frame queue first, then the world's attached buffers. The order matters and is stated
    // here rather than left to be inferred: a scene load recorded in the frame queue creates
    // entities, and creating them before the world flush means the flush sees one world rather
    // than two states of it.
    clock_.advance();
    if (tree_created_) {
        const FrameFlushReport report = commands_.flush(tree_);
        tick_stats_.frame_commands_applied = report.applied;
        tick_stats_.frame_commands_failed = report.failed;
    }
    const auto ingested = world_.flush();
    if (!ingested) {
        return Unexpected<Error>(ingested.error());
    }
    close_phase(TickPhase::IngestCommands);

    // --- RunSystems -------------------------------------------------------------------------
    for (const ecs::Stage stage : kFixedStages) {
        if (Status ran = run_stage(stage, jobs); !ran) {
            return Unexpected<Error>(ran.error());
        }
    }
    close_phase(TickPhase::RunSystems);

    // --- DrainTasks -------------------------------------------------------------------------
    //
    // `ecs::Schedule::run` waits for its batches before returning, so the graph is already drained
    // by the time the loop above ends. The phase is measured anyway — at zero — because a report
    // whose phases appear only when they cost something is a report that cannot be compared
    // between two runs.
    close_phase(TickPhase::DrainTasks);

    // --- MergeStructural --------------------------------------------------------------------
    const auto merged = world_.flush();
    if (!merged) {
        return Unexpected<Error>(merged.error());
    }
    close_phase(TickPhase::MergeStructural);

    // --- CommitEvents -----------------------------------------------------------------------
    //
    // The tree's lifecycle callbacks. The scene module is explicit that they cannot fire at the
    // moment of the write — a subtree attaches one edge at a time and `onReady` is defined as
    // running after all children are ready — so this is where they land, once, per tick.
    if (tree_created_) {
        if (Status pumped = tree_.pump(); !pumped) {
            return Unexpected<Error>(pumped.error());
        }
    }
    close_phase(TickPhase::CommitEvents);

    // --- Commit -----------------------------------------------------------------------------
    determinism::CommitRecord record;
    record.point = clock_.now();
    record.commands_applied = tick_stats_.frame_commands_applied;
    record.structural_changes = world_.structural_changes() - structural_before;
    tick_stats_.structural_changes = record.structural_changes;

    if (config_.hash.due(record.point.tick)) {
        const Nanoseconds hash_began = diagnostic_now();
        if (Status hashed = take_hash(); !hashed) {
            return Unexpected<Error>(hashed.error());
        }
        record.hash = hash_tree_.root_hash();
        record.hashed = true;
        tick_stats_.hash_ns = static_cast<u64>(diagnostic_now() - hash_began);
    }

    const Nanoseconds tick_ended = diagnostic_now();
    record.duration_ns = static_cast<u64>(tick_ended - tick_began);
    tick_stats_.total_ns = record.duration_ns;

    const auto committed = commit_.commit(record);
    close_phase(TickPhase::Commit);
    if (!committed) {
        return Unexpected<Error>(committed.error());
    }
    return committed.value();
}

Status Simulation::frame(f32 alpha, jobs::JobSystem* jobs) noexcept {
    if (!closed_) {
        return fail(ErrorCode::Unavailable, "Simulation::finalize_registration() has not run");
    }
    if (tree_created_) {
        // The tree is told the two deltas rather than reading a clock, which is the scene module's
        // own rule arriving from the other side: `SceneTree::set_time_step` exists precisely so
        // that the tree never holds one.
        tree_.set_time_step(clock_.delta_seconds(), clock_.delta_seconds());
    }

    for (const ecs::Stage stage : kFrameStages) {
        if (Status ran = run_stage(stage, jobs); !ran) {
            return ran;
        }
    }

    // The frame flush point, which `engine-architecture` names beside the per-stage ones.
    const auto flushed = world_.flush();
    if (!flushed) {
        return Unexpected<Error>(flushed.error());
    }
    if (tree_created_) {
        const FrameFlushReport report = commands_.flush(tree_);
        if (report.failed != 0) {
            return report.first_error;
        }
        if (Status pumped = tree_.pump(); !pumped) {
            return pumped;
        }
    }

    // `alpha` reaches no consumer at M2: the Render stage is empty until M3. It is a parameter
    // rather than something the render half fetches, so that the seam M3 fills is already shaped
    // the way the requirement wants — transforms are rendered at the interpolated pose using an
    // alpha they were handed.
    (void)alpha;
    return ok();
}

Expected<u64, Error> Simulation::hash_now() noexcept {
    if (!closed_) {
        return fail(ErrorCode::Unavailable, "Simulation::finalize_registration() has not run");
    }
    const Status hashed = take_hash();
    if (!hashed) {
        return Unexpected<Error>(hashed.error());
    }
    return hash_tree_.root_hash();
}

determinism::Epoch Simulation::reset_epoch(determinism::EpochReason reason, u64 tick) noexcept {
    return clock_.reset(reason, tick);
}

}  // namespace cy::runtime
