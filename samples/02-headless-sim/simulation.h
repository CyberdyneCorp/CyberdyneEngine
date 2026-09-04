// The two systems, the state schema they are hashed through, and the reproduction check.
// samples/02-headless-sim, tasks 5.1 and 5.3.
//
// WHAT MAKES A SYSTEM HERE DETERMINISTIC, AND NONE OF IT IS A CONVENTION:
//
//   * time comes from `Simulation::clock()`, which is handed out by const reference and has no
//     member that reads a wall clock — there is no expression in this file that could reach one;
//   * randomness comes from a stream named at startup and drawn by (point, entity, index), so a
//     draw is a pure function of where the simulation is and which entity is asking. There is no
//     generator with a hidden counter, and running the systems in parallel cannot reorder anything;
//   * the query IS the access declaration (`<cy/ecs/system.h>`), so the parallelism the scheduler
//     derives cannot drift away from what the body actually touches.
//
// WHY THE SCHEMA IS DECLARED BY HAND HERE. Two of the three things this world's state consists of
// are invisible to `declare_reflected_components()`:
//
//   `Placement` holds a `cy::Transform` as one opaque forty-byte field, because M1's reflection has
//   no vector kind. `FieldKind::Unsupported` is not hashable — deliberately, since hashing raw
//   structure memory is on `simulation-and-determinism`'s forbidden list — so the ten floats inside
//   it are declared explicitly, by offset, with the kind they actually are.
//
//   `scene::LocalTransform` is a built-in registered by name with no `TypeInfo` at all. Left alone,
//   every node in the tree would be counted in `WorldHashReport::subjects_undeclared` and the
//   authored hierarchy would contribute nothing to the hash. `StateSchema::declare()` is the route
//   the scene module's README names for exactly this, and this file is its first caller.
//
// Both are a gap in M1's reflection rather than in this design, and both close when a `Vec3` field
// kind and annotated headers for the scene's built-ins arrive.

#ifndef CY_SAMPLE_HEADLESS_SIM_SIMULATION_H
#define CY_SAMPLE_HEADLESS_SIM_SIMULATION_H

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/determinism/random.h>
#include <cy/ecs/query.h>
#include <cy/runtime/simulation.h>

#include "content.h"

namespace sample {

/// How much of the world the hash covers, as the sample's own report rather than as a claim.
struct SchemaReport {
    u32 subjects = 0;
    u32 declared_by_hand = 0;
    /// Component types with no declared schema. Non-zero is normal at M2 and is printed for that
    /// reason: `Parent`, `Children` and eleven of the scene's twelve built-ins have no `TypeInfo`.
    u32 undeclared = 0;
};

/// Declare every subject the hash should cover. Must run before `finalize_registration()`, which
/// declares the reflected remainder and freezes the schema.
[[nodiscard]] cy::Status declare_state_schema(cy::runtime::Simulation& simulation,
                                              const Components& ids) noexcept;

/// The systems, and the state they need. One object so that nothing here is a global: a system body
/// is a plain function pointer and reaches its state through `SystemContext::user`.
///
/// Held by the host for the simulation's whole lifetime — the schedule borrows both the queries and
/// this object.
class Systems {
public:
    /// What `drift` needs. Reaches the clock and the stream, and nothing else.
    ///
    /// Public because a system body is a plain function pointer that reaches its state through
    /// `SystemContext::user`, so the bodies are free functions and the state they cast to has to be
    /// nameable from outside the class.
    struct DriftState {
        const cy::runtime::Simulation* simulation = nullptr;
        cy::determinism::RandomStream stream;
        cy::ecs::ComponentTypeId drift = cy::ecs::kInvalidComponent;
        cy::ecs::Query* query = nullptr;
    };

    /// What `sweep` needs: the tree, the components it writes, and the nodes it moves.
    struct SweepState {
        const cy::runtime::Simulation* simulation = nullptr;
        cy::scene::SceneTree* tree = nullptr;
        cy::ecs::ComponentTypeId local_transform = cy::ecs::kInvalidComponent;
        cy::Span<const cy::ecs::Entity> batteries;
    };

    Systems(cy::runtime::Simulation& simulation, const Components& ids,
            const NodeReport& nodes) noexcept;

    Systems(const Systems&) = delete;
    Systems& operator=(const Systems&) = delete;

    /// Build the queries, name the random stream, and register both systems into the schedule.
    [[nodiscard]] cy::Status install() noexcept;

    [[nodiscard]] cy::u32 system_count() const noexcept { return systems_; }

private:
    cy::runtime::Simulation* simulation_;
    Components ids_;
    DriftState drift_;
    SweepState sweep_;
    cy::ecs::Query drift_query_;
    cy::u32 systems_ = 0;
};

/// What the reproduction check found. Every field is printed.
struct ReproductionReport {
    /// The hash of the world as the run left it.
    cy::u64 settled = 0;
    /// The same world after `diverging_ticks` more ticks. Must differ, or the check proves nothing.
    cy::u64 diverged = 0;
    /// The same world after the snapshot taken before those ticks is restored. Must equal
    /// `settled`.
    cy::u64 restored = 0;
    cy::u64 snapshot_bytes = 0;
    cy::u64 snapshot_entities = 0;
    cy::u32 diverging_ticks = 0;
    bool moved = false;    ///< `diverged` differs from `settled`
    bool matches = false;  ///< `restored` equals `settled`
    /// Where the restored world first disagreed, when it did. Empty otherwise.
    char divergence[192] = {};
};

/// Hash the world, run `diverging_ticks` more ticks, restore the snapshot, and hash again.
///
/// This is the artefact's claim about snapshot restore, and it is deliberately the narrow one: the
/// capture and the restore round-trip the world's state exactly. It is NOT "re-running from a
/// restored snapshot reproduces the run" — that needs the clock rewound to the same epoch and tick,
/// and `Simulation` exposes `reset_epoch()`, which by design enters a NEW epoch. The epoch is mixed
/// into every random draw, so replaying under a new one is a different simulation and is meant to
/// be. Rewinding a session to replay it is M9's, and the seam it needs is a public `resume()`.
[[nodiscard]] cy::Status check_snapshot_restore(cy::runtime::Simulation& simulation,
                                                cy::u32 diverging_ticks,
                                                ReproductionReport& report) noexcept;

/// Hash the world as it stands, filling `tree` with the hierarchy behind the number.
[[nodiscard]] cy::Expected<cy::u64, cy::Error> hash_world_now(
    cy::runtime::Simulation& simulation, cy::determinism::StateHashTree& tree,
    cy::runtime::WorldHashReport& report) noexcept;

/// Print the top of a hash tree: the root, its children, and their children. Not the whole tree —
/// the point of a hierarchy is that a divergence is narrowed to a subtree, and the top is where a
/// person starts looking.
void print_hash_tree(const cy::determinism::StateHashTree& tree, const char* tag) noexcept;

}  // namespace sample

#endif  // CY_SAMPLE_HEADLESS_SIM_SIMULATION_H
