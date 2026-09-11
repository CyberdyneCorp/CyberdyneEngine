#pragma once
// THE FOUR-PLAYER SESSION, OVER A NETWORK THAT LOSES PACKETS. M9 task 6.1.
//
// ================================================================================================
// THE CLAIM IS "UNDER PACKET LOSS", SO THE LOSS IS INJECTED AND MEASURED
// ================================================================================================
//
// `docs/ROADMAP.md`'s artefact sentence is "a four-player session over a simulated adverse network:
// prediction, reconciliation and rollback under packet loss". A sample that ran a clean network and
// asserted that it would have worked under a dirty one would be asserting the thing it was asked to
// demonstrate. So `LocalNetwork` is given real conditions — latency, jitter, loss, duplication and
// reordering — and the number of datagrams it actually destroyed is one of the artefact's figures.
//
// It is SEEDED, which is the only reason a lossy test is worth running: two runs at one seed drop
// the same datagrams at the same moments, so a figure here is a measurement rather than a draw.
// `--runs 2` re-runs the whole session and requires every figure to be identical.
//
// ================================================================================================
// THE TOPOLOGY, AND WHY THE HOST IS THE ONE THAT WRITES THE LOG
// ================================================================================================
//
//   four clients  ──inputs, UNRELIABLE, channel 0──▶  host
//         ▲                                            │
//         └──authoritative intents, RELIABLE ORDERED, channel 1──┘
//
// A player's input is sent unreliably and on time or not at all: retransmitting an input for a tick
// the host has already simulated would arrive as a correction rather than as an input, which is
// what rollback is for. So when a datagram is lost the host reaches its deadline with nothing for
// that player and SUBSTITUTES the last intent it had — `LatePolicy::RepeatPrevious`, and the
// substituted input is an ordinary command recorded by the ordinary seam, which is why a replay
// reproduces the substitution by replaying it rather than by re-running the decision.
//
// The host's broadcast is reliable-ordered because a client that never learned the truth would
// never reconcile, and "it diverged" and "it never found out" must not look alike. That is also
// what exercises `ReliableEndpoint`'s retransmission: the datagrams the network destroys on
// channel 1 are re-sent.
//
// **The host's `RecordLog` is the session's record.** One log, written once, by the machine whose
// simulation is authoritative — which is what makes acts 2, 3 and 4 readings of the same session
// rather than three separate demonstrations.
//
// ================================================================================================
// WHAT A CLIENT DOES, AND WHERE THE ROLLBACK IS
// ================================================================================================
//
// A client simulates the same tick the host does, at the same moment, and it cannot have the other
// three players' inputs yet — they are still in flight. So it PREDICTS: its own input exactly, and
// every remote player's by repeating the last intent it has been told about. When the host's
// authoritative packet for that tick finally arrives and disagrees with what the client predicted,
// the client ROLLS BACK to that tick and re-simulates forward to the present — with the
// authoritative inputs for the ticks the host has confirmed and fresh predictions beyond them.
//
// Every explosion the re-simulation offers is offered through `RollbackEngine::offer()`, which is
// the one door, and the ledger is what stops the ones that already played from playing again. The
// sample counts the duplicates ITSELF, in `Client::played_`, rather than trusting the ledger's own
// arithmetic: with the ledger's decision removed from `RollbackEngine::offer()` this program prints
// a non-zero `session_duplicate_effects` and exits non-zero, which is the mutation the ledger's
// exit criterion asks to be demonstrated.

#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/array.h>
#include <cy/networking/local_transport.h>
#include <cy/replay/ledger.h>
#include <cy/replay/log.h>
#include <cy/replay/readers.h>
#include <cy/replay/rollback.h>
#include <cy/replay/session.h>
#include <cy/replay/snapshot.h>

#include "world.h"

namespace cy::mp {

/// How far behind the wall clock the host simulates, in ticks. It has to exceed the network's
/// latency or every input would be late and the session would be substituting rather than playing;
/// it is stated here rather than tuned, and `SessionOptions::input_delay_ticks` is the dial.
inline constexpr u32 kDefaultInputDelay = 5;

/// The tick the session runs at. 60 Hz, so a tick is 16 ms of simulated time.
inline constexpr u64 kTickMicros = 16'667;
inline constexpr u64 kTickMillis = 16;

struct SessionOptions {
    u64 ticks = 180;
    u64 seed = 0x0910'C0FFULL;
    u32 input_delay_ticks = kDefaultInputDelay;
    u32 latency_ms = 40;
    u32 jitter_ms = 20;
    u32 loss_percent = 8;
    u32 duplication_percent = 2;
    u32 reorder_percent = 5;
    u32 checkpoint_every = 10;
    /// The crash replay buffer's window, in seconds of session. Zero disables the buffer, which is
    /// what `crash_ring_records()` returns zero to mean.
    u64 crash_window_seconds = 2;
};

/// What one client's reconciliation amounted to.
struct ClientReport {
    u32 rollbacks = 0;
    u32 refusals = 0;
    u32 ticks_resimulated = 0;
    u32 effects_offered = 0;
    u32 effects_suppressed = 0;
    u32 explosions_played = 0;
    /// **Explosions played twice for the same entity at the same tick.** Zero is the claim; it is
    /// counted here rather than inferred from the ledger's own numbers.
    u32 duplicate_effects = 0;
    u32 mispredicted_ticks = 0;
    /// The lowest tick this client's world was still holding a PREDICTED input for when the session
    /// ended. `ticks` means every tick was ultimately simulated from authoritative input, which is
    /// the condition under which convergence is a claim rather than a coincidence.
    u64 first_unconfirmed_tick = 0;
    /// One past the last tick this client has the host's authoritative inputs for. Equal to the
    /// session's length when the session ended with nothing still in flight.
    u64 frontier = 0;
    u64 final_hash = 0;
    bool converged = false;
};

/// What the whole session amounted to.
struct SessionReport {
    u64 ticks_simulated = 0;
    u64 datagrams_offered = 0;
    u64 datagrams_dropped = 0;
    u64 datagrams_duplicated = 0;
    u64 datagrams_delivered = 0;
    /// Player-ticks the host reached its deadline with nothing for. A direct consequence of loss.
    u32 substitutions = 0;
    u32 host_explosions = 0;
    u64 host_final_hash = 0;
    u32 records_written = 0;
    u32 records_dropped = 0;
    u32 checkpoints = 0;
    u32 crash_ring_capacity = 0;
    u32 crash_ring_size = 0;
    u64 crash_ring_overwritten = 0;
    ClientReport clients[kPlayers];

    [[nodiscard]] u32 rollbacks() const noexcept;
    [[nodiscard]] u32 duplicate_effects() const noexcept;
    [[nodiscard]] u32 converged_clients() const noexcept;
};

/// One line of the per-tick trace the driver paints its picture from.
struct TraceSample {
    u64 tick = 0;
    f32 x[kPlayers] = {};
    f32 y[kPlayers] = {};
    /// One bit per player: the host reached this tick's deadline with nothing from them.
    u32 substituted = 0;
    /// Rollbacks each client performed while this tick was the present one, and how many ticks
    /// each of them re-simulated doing it. This is the reconciliation as it happened rather than
    /// as a total, and it is what the artefact's picture is drawn from.
    u16 rollbacks[kPlayers] = {};
    u16 depth[kPlayers] = {};
    u64 host_hash = 0;
};

/// The session. Owns everything: the substrate, five transports, five worlds, the host's log, the
/// snapshot rings and the ledgers.
class NetworkedSession {
public:
    NetworkedSession(const SessionOptions& options,
                     const replay::CompatibilityManifest& manifest) noexcept;
    ~NetworkedSession();

    NetworkedSession(const NetworkedSession&) = delete;
    NetworkedSession& operator=(const NetworkedSession&) = delete;

    [[nodiscard]] bool build() noexcept;
    /// Play it. The whole session, wall tick by wall tick.
    [[nodiscard]] bool run() noexcept;

    [[nodiscard]] const SessionReport& report() const noexcept { return report_; }
    [[nodiscard]] Span<const TraceSample> trace() const noexcept { return trace_.span(); }
    /// The session's record. Valid after `run()`, and what acts 2, 3 and 4 read.
    [[nodiscard]] replay::RecordLog& log() noexcept { return log_; }
    [[nodiscard]] replay::SnapshotRing& checkpoints() noexcept { return checkpoints_; }
    [[nodiscard]] replay::CrashReplayBuffer& crash_ring() noexcept { return ring_; }
    [[nodiscard]] GameWorld& host_world() noexcept { return host_world_; }
    [[nodiscard]] const gameplay::FirewallArmingReport& arming() const noexcept {
        return host_world_.arming();
    }
    /// The last tick the host actually simulated. Not `options.ticks`: the host runs behind the
    /// wall clock by the input delay, and saying so is cheaper than a reader working it out.
    [[nodiscard]] u64 last_simulated_tick() const noexcept { return last_tick_; }

private:
    class Client;

    [[nodiscard]] bool link() noexcept;
    [[nodiscard]] bool send_inputs(u64 wall_tick) noexcept;
    [[nodiscard]] bool host_tick(u64 simulated) noexcept;
    void advance_transports(u64 now_ms) noexcept;
    [[nodiscard]] bool drain_host() noexcept;
    [[nodiscard]] bool broadcast(u64 simulated) noexcept;
    /// Close the tick: take each client's reconciliation for it and push the sample.
    [[nodiscard]] bool close_tick() noexcept;

    SessionOptions options_;
    replay::CompatibilityManifest manifest_;

    net::LocalNetwork network_;
    net::PeerId host_id_;
    net::PeerId client_ids_[kPlayers];
    net::LocalTransport* host_transport_ = nullptr;
    Array<net::LocalTransport*> client_transports_;
    Array<Client*> clients_;

    GameWorld host_world_;
    replay::RecordLog log_;
    replay::SessionRecorder recorder_;
    replay::SnapshotRing checkpoints_;
    replay::CrashReplayBuffer ring_;
    replay::SideEffectLedger host_ledger_;
    determinism::EpochCounter host_epochs_;
    replay::RollbackEngine host_engine_;

    /// The most recent intent the host has for each player, and whether this tick's arrived.
    Array<MoveIntent> arrived_;
    Array<u8> arrived_flag_;
    MoveIntent last_known_[kPlayers] = {};
    MoveIntent decided_[kPlayers] = {};
    u32 substituted_mask_ = 0;

    Array<TraceSample> trace_;
    TraceSample pending_;
    SessionReport report_;
    u64 last_tick_ = 0;
    u32 host_explosions_ = 0;
    bool built_ = false;

    static void on_host_effect(void* user, u64 kind, u64 instance) noexcept;
};

}  // namespace cy::mp
