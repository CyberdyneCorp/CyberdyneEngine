#pragma once
// The determinism policy, the session validation it drives, and the per-tick divergence probe.
// Task 4.2.6.
//
// `physics` — "Determinism": physics declares a POLICY rather than one blanket guarantee, the
// default is `SamePlatformDeterministic`, cross-platform determinism is NOT guaranteed, and "the
// engine SHALL provide a determinism test mode that hashes world state per tick to detect
// divergence" reporting "the tick number and the diverging body".
//
// ================================================================================================
// WHAT IS AND IS NOT CLAIMED HERE
// ================================================================================================
//
// CLAIMED, AND TESTED: the same binary, on this platform, from the same initial state and the same
// per-tick inputs, produces bit-identical results. `integration.physics_determinism` runs a scene
// twice in one process and compares the per-tick hash trees, and runs it a third time in a separate
// process invocation so that "identical" is not an artefact of a warm allocator.
//
// NOT CLAIMED, AND NOT TESTED: anything across platforms, architectures or compilers. That is M9's
// (`simulation-and-determinism`'s deterministic math path) and this milestone must not pretend
// otherwise. One machine, one toolchain, one architecture is what was measured.
//
// ================================================================================================
// WHY THE VALIDATION IS A FUNCTION AND NOT A COMMENT
// ================================================================================================
//
// `physics`: "A session declaring `CrossPlatform` or `Lockstep` while treating physics as
// authoritative SHALL be rejected at configuration time." A rule that lives only in prose is a rule
// that is discovered by a desync months later — which is the exact failure the requirement calls
// "load-bearing for networking". `validate_session()` below is the rejection, and it is called at
// world creation rather than at the first divergence.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/memory/array.h>
#include <cy/servers/physics/server.h>

namespace cy::physics {

/// The determinism a session requires, as `simulation-and-determinism` scopes it.
enum class SessionDeterminism : u8 {
    /// No cross-run guarantee is needed. Single-player, no replay.
    None = 0,
    /// The same binary on the same platform reproduces a recorded session.
    SamePlatform,
    /// Peers on different platforms must agree bit for bit.
    CrossPlatform,
    /// Every peer simulates every tick and the results must be identical.
    Lockstep,
};

const char* session_determinism_name(SessionDeterminism value) noexcept;

/// Whether the session treats physics results as part of authoritative state.
enum class PhysicsAuthority : u8 {
    /// Physics decides where things are, and that is replicated or recorded.
    Authoritative = 0,
    /// Debris, ragdolls and secondary effects. Excluded from the deterministic core.
    Presentation,
};

/// Reject a configuration whose determinism requirement the backend cannot meet.
///
/// `physics`' two rules, verbatim:
///   * a `CrossPlatform` or `Lockstep` session with authoritative physics is rejected, because no
///     backend here guarantees cross-platform determinism;
///   * a `SamePlatform` session with authoritative physics needs a backend whose policy is
///     `SamePlatformDeterministic`.
///
/// The escape hatch the requirement names is real and is spelled `PhysicsAuthority::Presentation`:
/// "physics MAY be classified `NonAuthoritative` and used for debris, ragdolls, and secondary
/// effects outside the deterministic core".
[[nodiscard]] Status validate_session(SessionDeterminism session, PhysicsAuthority authority,
                                      DeterminismPolicy backend) noexcept;

/// Where two runs of the same scene first disagreed.
struct PhysicsDivergence {
    bool diverged = false;
    /// The tick the mismatch was found on.
    u64 tick = 0;
    /// The body, as `BodyHandle::bits()`. Null when the disagreement was above the body level — a
    /// different body COUNT, say, which `shape_mismatch` distinguishes.
    BodyHandle body;
    bool shape_mismatch = false;
    u64 left_hash = 0;
    u64 right_hash = 0;
};

/// Records one hash per tick and compares two runs.
///
/// `physics` calls this "determinism test mode". It is a development tool, not a shipping one: it
/// keeps a whole `StateHashTree` per tick, which is a few hundred bytes per body per tick, and a
/// session that ran for an hour at 60 Hz would be gigabytes. `capacity` bounds it and the recorder
/// stops rather than growing, because a diagnostic that runs the machine out of memory has replaced
/// the bug it was looking for.
class DeterminismProbe {
public:
    DeterminismProbe(Allocator& allocator, u32 capacity) noexcept;
    ~DeterminismProbe();

    DeterminismProbe(const DeterminismProbe&) = delete;
    DeterminismProbe& operator=(const DeterminismProbe&) = delete;

    /// Hash the world as it stands and keep the tree under `tick`.
    [[nodiscard]] Status record(const PhysicsServer& server, WorldHandle world, u64 tick) noexcept;

    [[nodiscard]] u32 recorded() const noexcept { return static_cast<u32>(ticks_.size()); }
    [[nodiscard]] u64 hash_at(u32 index) const noexcept;
    [[nodiscard]] u64 tick_at(u32 index) const noexcept;

    /// Compare, tick by tick, and report the first disagreement.
    ///
    /// The report names the body because `StateHashTree::compare` matches children by `(level, id)`
    /// and `hash_state` opens one `HashLevel::Entity` node per body with the handle's bits as the
    /// id. The ids are what makes an inserted body a shape mismatch on THAT body rather than a
    /// value mismatch on every body after it — the walk ORDER only has to be stable across runs,
    /// which is why `hash_state` walks creation order rather than sorting.
    [[nodiscard]] static PhysicsDivergence compare(const DeterminismProbe& left,
                                                   const DeterminismProbe& right) noexcept;

private:
    /// One tree per recorded tick. `Array<StateHashTree>` is not possible — the tree is
    /// non-copyable and non-movable — so the trees are heap-allocated and owned by index.
    Array<determinism::StateHashTree*> trees_;
    Array<u64> ticks_;
    Allocator* allocator_;
    u32 capacity_;
};

}  // namespace cy::physics
