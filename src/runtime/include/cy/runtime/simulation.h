#pragma once
// The simulation: the ECS world, its scene façade, the tick pipeline, and the commit boundary.
// Tasks 4.1.1, 4.1.2, 4.1.3 and 4.2.3.
//
// This is where `engine-architecture`'s "ECS core with a scene-graph façade" stops being two
// modules and becomes one running thing. `ecs::World` is the authoritative storage,
// `scene::SceneTree` is the handle façade over it, and neither knows about the frame; this class
// owns both, installs the tree's systems into the schedule, and drives the eight stages in the two
// halves the loop defines.
//
// --- THE TICK, PHASE BY PHASE --------------------------------------------------------------------
//
// `simulation-and-determinism` requires a defined commit boundary "reached after: commands are
// ingested, systems execute, the task graph drains, per-worker structural buffers merge
// deterministically, events commit, and the state version increments", and requires **every**
// consumer of authoritative state to key off it. `step()` is exactly that list, in that order, with
// `determinism::TickPhase` as the shared vocabulary:
//
//   IngestCommands    the frame-scoped queue is drained, then the world's attached buffers.
//   RunSystems        PreSimulation, Physics, Simulation, PostSimulation — the four fixed-step
//                     stages, each flushing its own systems' command buffers as `ecs-core`
//                     requires.
//   DrainTasks        the schedule has already waited; the phase exists so a timing report can
//                     attribute the wait rather than fold it into the last stage.
//   MergeStructural   `World::flush()` — anything recorded outside a system's own buffer.
//   CommitEvents      `SceneTree::pump()`: the tree-shape and enablement callbacks, which the scene
//                     module documents as belonging after the stage flush and not at the write.
//   Commit            the state version increments and every observer is told. Past this line the
//                     tick's state is authoritative.
//
// A consumer of authoritative state does not get a hook anywhere else. That is the design: "one
// moment, many consumers" is only true if there is one call site, and the way to keep it true is
// for a second one to require an edit to this file.
//
// --- THE CLOCK BOUNDARY, WHICH IS EASY TO BREAK --------------------------------------------------
//
// `determinism::SimulationClock` cannot read a wall clock and neither can anything a system reaches
// through this class. The tick's *duration*, which the diagnostics requirement asks for, therefore
// comes from a function pointer the host supplies in the configuration — `diagnostic_clock` — and
// it is private: it is read in exactly two places in simulation.cpp and appears in no interface a
// system is handed. With no such function every duration is reported as zero, which is the honest
// value for a build that chose not to measure.

#include <cy/core/base/expected.h>
#include <cy/core/determinism/clock.h>
#include <cy/core/determinism/commit.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/determinism/provider.h>
#include <cy/core/determinism/random.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/core/jobs/job_system.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>
#include <cy/runtime/frame_commands.h>
#include <cy/runtime/state_hash.h>
#include <cy/scene/tree.h>

namespace cy::runtime {

/// Reads a monotonic counter, for diagnostics only. See the header comment on why this is a
/// parameter rather than a call.
using DiagnosticClockFn = Nanoseconds (*)(void* user) noexcept;

struct SimulationConfig {
    /// Named in diagnostics, in a snapshot header, and as the root of the state hash.
    const char* world_name = "world";
    u32 chunk_bytes = static_cast<u32>(kDefaultChunkBytes);

    determinism::ClockConfig clock;

    /// The session seed every named random stream is derived from. Zero is a legal seed and is the
    /// default, because a seed invented by the engine is a seed nobody recorded.
    u64 session_seed = 0;

    /// How often the state hash is taken at the commit boundary. `Never` by default: hashing is a
    /// full walk at M2 (state_hash.h), and a cost that is not asked for should not be paid.
    determinism::HashSchedule hash;

    /// False builds the world without a node façade. A headless simulation over bare entities is a
    /// legitimate configuration, and `engine-architecture` says so outright: "Not every entity
    /// needs a node."
    bool create_scene_tree = true;

    DiagnosticClockFn diagnostic_clock = nullptr;
    void* diagnostic_clock_user = nullptr;
};

/// What one tick cost, by phase. `simulation-and-determinism`'s "Simulation diagnostics" asks for
/// tick duration by phase, commit boundary cost and hash cost; this is that, and every number is
/// zero when the configuration supplied no diagnostic clock.
struct TickStats {
    u64 phase_ns[determinism::kTickPhaseCount] = {};
    u64 hash_ns = 0;
    u64 total_ns = 0;
    u32 frame_commands_applied = 0;
    u32 frame_commands_failed = 0;
    u64 structural_changes = 0;
};

/// One world, its façade, its schedule, and the boundary they commit at.
class Simulation {
public:
    Simulation(Allocator& allocator, const SimulationConfig& config) noexcept;
    ~Simulation();

    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    /// Bring up the world, the tree and the built-in state providers. Separate from the constructor
    /// for the reason `World::initialize` and `SceneTree::initialize` are: it allocates, and a
    /// constructor under `-fno-exceptions` cannot report failure.
    [[nodiscard]] Status initialize() noexcept;
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    /// Declare a schema for every reflected component, finalise the provider registry, freeze the
    /// schema and build the schedule.
    ///
    /// The one call that closes registration. `simulation-and-determinism`: "Registries whose
    /// contents affect simulation SHALL be finalised in a deterministic order derived from stable
    /// identifiers **before simulation begins**." `step()` refuses until this has run, rather than
    /// finalising lazily on the first tick — a registry that closed itself when the first tick
    /// happened to arrive would close at a different point in a run that ticked earlier.
    [[nodiscard]] Status finalize_registration() noexcept;
    [[nodiscard]] bool registration_closed() const noexcept { return closed_; }

    [[nodiscard]] ecs::World& world() noexcept { return world_; }
    [[nodiscard]] const ecs::World& world() const noexcept { return world_; }
    /// Present only when the configuration asked for one; null otherwise.
    [[nodiscard]] scene::SceneTree* tree() noexcept { return tree_created_ ? &tree_ : nullptr; }
    [[nodiscard]] ecs::Schedule& schedule() noexcept { return schedule_; }
    [[nodiscard]] FrameCommandQueue& commands() noexcept { return commands_; }

    /// The clock, by const reference and never otherwise: a system may ask what tick it is and has
    /// no way to move it.
    [[nodiscard]] const determinism::SimulationClock& clock() const noexcept { return clock_; }
    [[nodiscard]] const determinism::RandomSource& random() const noexcept { return random_; }
    [[nodiscard]] determinism::StateProviderRegistry& providers() noexcept { return providers_; }
    [[nodiscard]] determinism::CommitBoundary& commit_boundary() noexcept { return commit_; }
    [[nodiscard]] determinism::StateSchema& schema() noexcept { return schema_; }
    [[nodiscard]] const determinism::StateSchema& schema() const noexcept { return schema_; }

    /// The last committed tick. What "a system queries authoritative state" observes — never a tick
    /// in progress.
    [[nodiscard]] const determinism::CommitRecord& committed() const noexcept {
        return commit_.last();
    }
    [[nodiscard]] const TickStats& last_tick() const noexcept { return tick_stats_; }
    /// The hash tree from the most recent hashed commit. Empty when none has been taken; this is
    /// what a divergence comparison and a narrowing report are run against.
    [[nodiscard]] const determinism::StateHashTree& last_hash_tree() const noexcept {
        return hash_tree_;
    }
    [[nodiscard]] const WorldHashReport& last_hash_report() const noexcept { return hash_report_; }

    /// Feed the frame's measured elapsed time to the clock and learn how many fixed ticks it
    /// bought.
    ///
    /// The runtime's door to the clock, and the **only** mutable one on this class: `clock()`
    /// returns a const reference, so a system can ask what tick it is and has no way to move it.
    /// Wall-clock time enters the simulation here and nowhere else.
    [[nodiscard]] determinism::FrameTicks begin_frame(Nanoseconds elapsed_ns) noexcept {
        return clock_.accumulate(elapsed_ns);
    }

    /// Run one fixed simulation tick and cross the commit boundary.
    ///
    /// `jobs` may be null, in which case every stage runs serially on the calling thread. The
    /// result is identical either way — that is what deriving parallelism from declared access buys
    /// — and it is what a test without a job system runs.
    [[nodiscard]] Expected<determinism::CommitRecord, Error> step(jobs::JobSystem* jobs) noexcept;

    /// Run the variable-rate half of a frame: the Frame, Animation, UI and Render stages, then the
    /// frame flush point.
    ///
    /// `alpha` is the interpolation alpha the clock computed. Nothing in M2 reads it — there is no
    /// renderer — and it is passed rather than fetched so that the seam M3 fills is already the
    /// right shape: the render half is handed the alpha, it does not go looking for a clock.
    [[nodiscard]] Status frame(f32 alpha, jobs::JobSystem* jobs) noexcept;

    /// Take a hash now, outside the schedule. `HashFrequency::OnDemand`'s other half, and what a
    /// test and a debugger use.
    [[nodiscard]] Expected<u64, Error> hash_now() noexcept;

    /// Leave the current epoch — a checkpoint restore, a world reload, a hot reload. The tick the
    /// timeline continues from is explicit, because a restore rewinds it and a hot reload does not.
    [[nodiscard]] determinism::Epoch reset_epoch(determinism::EpochReason reason,
                                                 u64 tick) noexcept;

private:
    /// The clock, wrapped so that a checkpoint captures where the simulation is. Declared here
    /// rather than in a separate header because it exists to be registered, not to be reused.
    class ClockProvider final : public determinism::StateProvider {
    public:
        explicit ClockProvider(Simulation& owner) noexcept : owner_(&owner) {}
        [[nodiscard]] const char* name() const noexcept override { return "simulation.clock"; }
        [[nodiscard]] determinism::Participates participation() const noexcept override;
        [[nodiscard]] Status hash(determinism::StateHashTree& tree) const noexcept override;

    private:
        Simulation* owner_;
    };

    /// The session seed and the mixer version. Hashed, so that two runs with different seeds
    /// diverge at the root with a named subsystem rather than somewhere in the entity data.
    class RandomProvider final : public determinism::StateProvider {
    public:
        explicit RandomProvider(Simulation& owner) noexcept : owner_(&owner) {}
        [[nodiscard]] const char* name() const noexcept override { return "simulation.random"; }
        [[nodiscard]] determinism::Participates participation() const noexcept override;
        [[nodiscard]] Status hash(determinism::StateHashTree& tree) const noexcept override;

    private:
        Simulation* owner_;
    };

    [[nodiscard]] Nanoseconds diagnostic_now() const noexcept;
    [[nodiscard]] Status run_stage(ecs::Stage stage, jobs::JobSystem* jobs) noexcept;
    [[nodiscard]] Status take_hash() noexcept;

    SimulationConfig config_;
    ecs::World world_;
    scene::SceneTree tree_;
    ecs::Schedule schedule_;
    FrameCommandQueue commands_;

    determinism::SimulationClock clock_;
    determinism::RandomSource random_;
    determinism::CommitBoundary commit_;
    determinism::StateProviderRegistry providers_;
    determinism::StateSchema schema_;
    determinism::StateHashTree hash_tree_;
    WorldHashReport hash_report_;

    ClockProvider clock_provider_{*this};
    RandomProvider random_provider_{*this};

    TickStats tick_stats_;
    bool initialized_ = false;
    bool tree_created_ = false;
    bool closed_ = false;
};

}  // namespace cy::runtime
