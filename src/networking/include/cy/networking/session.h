// SPDX-License-Identifier: MIT
#pragma once
// The dedicated-server host and the client host: a transport, a world, replication and the
// command stream, driven at a fixed tick. Issue #82.
//
// ================================================================================================
// THE TWO LOOPS
// ================================================================================================
//
//   ServerHost::update(now)                       ClientHost::update(now)
//     transport: receive, handshake, events          transport: receive, events
//     joins: compatibility scope -> admit/refuse     join: send scope once connected
//     acks -> WorldReplicator::acknowledge           snapshots -> ReplicaWorld::apply, ack each
//     commands -> validate, queue by tick            commands submitted -> one message per update
//     for each tick due at the fixed rate:           transport: flush
//       world.advance_version()
//       simulate(world, tick, commands due)
//       gather; build and send one snapshot per peer
//     transport: flush
//
// `ServerHost` is headless by construction — this module links no renderer, audio, interface or
// GPU (see server.h) — and owns no simulation: the application's `SimulateFn` is the simulation,
// handed the world, the tick and the commands due at it, exactly as a single-player game steps.
//
// ================================================================================================
// COMMANDS ARE INPUT, AND THE SERVER'S RECORD OF THEM IS THE WHOLE OF ITS INPUT
// ================================================================================================
//
// A client sends intent, never state: a `NetCommand` names the tick it was issued for, a stable
// command type, an optional target and up to `gameplay::kMaxCommandPayload` bytes. The server
// validates each one on receipt — rate (at most `max_commands_per_tick` issued for any one tick,
// the excess refused and counted against the peer), order (the tick a peer names never goes
// backwards), timing (inside the command window around the current tick) and size — then applies
// it at its tick, or at the current tick when it arrived late. Every command applied is appended to
// `ServerInputLog` with the tick it was APPLIED at and the peer it came from, in the order the
// simulation saw it: (tick, peer slot, arrival). Feeding that log to a fresh server reproduces the
// run bit for bit, with no network at all, which is `gameplay-framework`'s "the log is the whole of
// intent" for a networked session.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/world.h>
#include <cy/networking/mode.h>
#include <cy/networking/scheduler.h>
#include <cy/networking/server.h>
#include <cy/networking/transport.h>
#include <cy/networking/world_replication.h>

namespace cy::net {

/// The messages a session sends, by their first byte.
enum class SessionMessage : u8 {
    Join = 1,
    JoinAccepted = 2,
    JoinRefused = 3,
    Snapshot = 4,
    Ack = 5,
    Commands = 6,
};

/// The channels a session uses. Fixed, so a server and a client cannot disagree about them.
namespace session_channel {
/// Join, accept and refuse: reliable and ordered.
inline constexpr ChannelId kControl = 0;
/// Snapshots, server to client: unreliable, repeated rather than retransmitted.
inline constexpr ChannelId kSnapshots = 1;
/// Snapshot acknowledgements, client to server: unreliable, each carrying 33 snapshots' worth.
inline constexpr ChannelId kAcks = 2;
/// Gameplay commands, client to server: reliable and ordered.
inline constexpr ChannelId kCommands = 3;
}  // namespace session_channel

/// A command's payload, at most: `gameplay::kMaxCommandPayload`, so a gameplay command fits.
inline constexpr u32 kMaxNetCommandPayload = 48;

/// One gameplay command on the wire.
struct NetCommand {
    /// The tick the client issued it for.
    u64 tick = 0;
    /// `gameplay::CommandDeclaration::stable_id` — persistent, never a runtime index.
    u32 type = 0;
    /// The entity it addresses, as a network id. Null for a session-level command.
    NetworkId target;
    u16 payload_size = 0;
    u8 payload[kMaxNetCommandPayload] = {};
};

/// A command the server applied, as the simulation sees it — or a peer joining or leaving, which
/// changes the world just as a command does and is therefore part of the same record.
struct AppliedCommand {
    enum class Kind : u8 {
        Command = 0,
        /// `PeerFn` ran with `joined == true` at this tick, before any command.
        Joined,
        Left,
    };

    Kind kind = Kind::Command;
    u64 tick = 0;
    PeerId peer;
    NetCommand command;
    /// `command.target` resolved to the server's entity, or null.
    ecs::Entity target;
};

/// Every command the server applied, in order.
class ServerInputLog {
public:
    explicit ServerInputLog(Allocator& allocator) noexcept : entries_(allocator) {}

    [[nodiscard]] Status append(const AppliedCommand& command) noexcept {
        return entries_.push_back(command);
    }
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(entries_.size()); }
    [[nodiscard]] const AppliedCommand& at(u32 index) const noexcept { return entries_[index]; }
    [[nodiscard]] Span<const AppliedCommand> entries() const noexcept { return entries_.span(); }
    /// A value identity over every applied command. Two runs that applied the same input agree.
    [[nodiscard]] u64 hash() const noexcept;

private:
    Array<AppliedCommand> entries_;
};

/// The simulation: one fixed step of `world` at `tick`, given the commands due at it.
using SimulateFn = void (*)(void* user, ecs::World& world, u64 tick,
                            Span<const AppliedCommand> commands) noexcept;

/// A peer was admitted or left. The application creates a player's entities here, owned by `peer`.
using PeerFn = void (*)(void* user, PeerId peer, bool joined) noexcept;

/// How a dedicated server runs: its mode and scope, its tick, its budget, its limits on input, and
/// the application's simulation and join callbacks.
struct ServerHostConfig {
    NetworkMode mode = NetworkMode::SnapshotAuthoritative;
    CompatibilityScope scope{};
    u32 maximum_peers = 16;
    u32 tick_rate_hz = 30;
    /// A snapshot every this many ticks.
    u32 snapshot_interval_ticks = 1;
    BandwidthBudget budget{};
    /// At most this many commands issued for any one tick, per peer; the excess is refused.
    u32 max_commands_per_tick = 8;
    /// A command naming a tick more than this far from the current one is refused.
    u32 command_window_ticks = 64;
    SimulateFn simulate = nullptr;
    void* simulate_user = nullptr;
    PeerFn on_peer = nullptr;
    void* on_peer_user = nullptr;
};

/// What the server knows about one admitted peer, for diagnostics and tests.
struct ServerPeerStats {
    PeerId peer;
    u64 snapshots_sent = 0;
    u64 snapshot_bytes = 0;
    u64 commands_accepted = 0;
    /// Refused for rate, timing or size. A peer with a non-zero count here is flagged.
    u64 commands_refused = 0;
    bool flagged = false;
};

/// The dedicated server's loop — see the header comment.
class ServerHost {
public:
    ServerHost(Allocator& allocator, Transport& transport, ecs::World& world,
               WorldReplicator& replicator, const ServerHostConfig& config) noexcept;
    ~ServerHost();

    ServerHost(const ServerHost&) = delete;
    ServerHost& operator=(const ServerHost&) = delete;

    /// Receive, admit, run every tick that is due by `now_us`, send, flush.
    void update(u64 now_us) noexcept;

    /// One fixed step, now. `update()` calls it at the tick rate; a test or a replay calls it
    /// directly.
    void step() noexcept;

    /// Queue a command as though `peer` had sent it — the door a listen server's own player uses.
    [[nodiscard]] Status inject(PeerId peer, const NetCommand& command) noexcept;

    /// One fixed step that applies exactly `entries` — joins, leaves and commands, in order — in
    /// place of what arrived. Feeding a recorded `ServerInputLog` through this, tick by tick, into
    /// a server over a fresh world reproduces the recorded run with no network.
    void replay_step(Span<const AppliedCommand> entries) noexcept;

    [[nodiscard]] u64 tick() const noexcept { return tick_; }
    [[nodiscard]] const ServerInputLog& input_log() const noexcept { return log_; }
    [[nodiscard]] u32 peer_count() const noexcept;
    [[nodiscard]] const ServerPeerStats* peer_stats(PeerId peer) const noexcept;
    [[nodiscard]] const DedicatedServer& admission() const noexcept { return admission_; }
    [[nodiscard]] u64 refused_joins() const noexcept { return refused_joins_; }

private:
    struct Slot;
    struct Pending {
        PeerId peer;
        NetCommand command;
        u64 arrival = 0;
    };

    [[nodiscard]] Slot* find(PeerId peer) noexcept;
    void handle_events() noexcept;
    void handle_message(PeerId peer, ChannelId channel, Span<const u8> bytes) noexcept;
    void handle_join(PeerId peer, Span<const u8> bytes) noexcept;
    void handle_commands(Slot& slot, Span<const u8> bytes) noexcept;
    [[nodiscard]] bool admit_command(Slot& slot, const NetCommand& command) const noexcept;
    void drop(PeerId peer) noexcept;
    void collect_due(Array<AppliedCommand>& out) noexcept;
    void run_step(Span<const AppliedCommand> due) noexcept;
    void replicate() noexcept;
    void send_refusal(PeerId peer, const AdmissionRejection& rejection) noexcept;

    Allocator* allocator_;
    Transport* transport_;
    ecs::World* world_;
    WorldReplicator* replicator_;
    ServerHostConfig config_;
    DedicatedServer admission_;
    Array<Slot*> slots_;
    Array<Pending> pending_;
    /// Joins and leaves since the last step, applied at the start of the next one.
    Array<AppliedCommand> membership_;
    Array<AppliedCommand> due_;
    struct Farewell {
        PeerId peer;
        u64 at_us = 0;
    };
    /// Refused peers, disconnected once their refusal has had time to arrive.
    Array<Farewell> farewells_;
    u64 now_us_ = 0;
    ServerInputLog log_;
    Array<u8> message_;
    u64 tick_ = 0;
    u64 next_tick_us_ = 0;
    bool started_ = false;
    u64 arrivals_ = 0;
    u64 refused_joins_ = 0;
};

/// Where a client host is in joining a session.
enum class ClientPhase : u8 {
    Idle = 0,
    /// The transport handshake is in progress.
    Connecting,
    /// Connected; the join message is out and the server has not answered.
    Joining,
    Joined,
    /// The server refused the join; `refusal()` says why.
    Refused,
    Disconnected,
};

const char* client_phase_name(ClientPhase phase) noexcept;

/// What a client host presents when it joins.
struct ClientHostConfig {
    CompatibilityScope scope{};
};

/// The client's loop — see the header comment.
class ClientHost {
public:
    ClientHost(Allocator& allocator, Transport& transport, ReplicaWorld& replica,
               const ClientHostConfig& config) noexcept;

    [[nodiscard]] Status connect(const char* address) noexcept;
    void disconnect() noexcept;

    /// Receive and apply snapshots, acknowledge them, send submitted commands, flush.
    void update(u64 now_us) noexcept;

    /// Queue a command for the server. Sent with the next `update()`.
    [[nodiscard]] Status submit(const NetCommand& command) noexcept;

    [[nodiscard]] ClientPhase phase() const noexcept { return phase_; }
    [[nodiscard]] AdmissionRejection refusal() const noexcept { return refusal_; }
    [[nodiscard]] PeerId server() const noexcept { return server_; }
    /// This client as the server knows it — the owner its own entities carry.
    [[nodiscard]] PeerId self() const noexcept { return self_; }
    /// The server's tick, as of the newest snapshot applied.
    [[nodiscard]] u64 server_tick() const noexcept { return server_tick_; }
    [[nodiscard]] u64 snapshots_applied() const noexcept { return applied_; }
    [[nodiscard]] u64 snapshot_bytes() const noexcept { return snapshot_bytes_; }

private:
    void handle_events() noexcept;
    void handle_message(ChannelId channel, Span<const u8> bytes) noexcept;
    void handle_control(SessionMessage type, Span<const u8> body) noexcept;
    void handle_snapshot(Span<const u8> body) noexcept;
    void send_join() noexcept;
    void send_ack() noexcept;
    void send_commands() noexcept;

    Transport* transport_;
    ReplicaWorld* replica_;
    ClientHostConfig config_;
    Array<NetCommand> outbox_;
    Array<u8> message_;
    PeerId server_;
    PeerId self_;
    ClientPhase phase_ = ClientPhase::Idle;
    AdmissionRejection refusal_{};
    u64 server_tick_ = 0;
    u64 applied_ = 0;
    u64 snapshot_bytes_ = 0;
    bool ack_due_ = false;
};

// --- The session messages, exposed for tests and tools -------------------------------------------

/// Encode `commands` as one `Commands` message.
[[nodiscard]] Status encode_commands(Span<const NetCommand> commands, Array<u8>& out) noexcept;
/// Decode a `Commands` message. Refuses — rather than truncates — a count or a payload that does
/// not fit the bytes.
[[nodiscard]] Status decode_commands(Span<const u8> bytes, Array<NetCommand>& out) noexcept;

}  // namespace cy::net
