// The four-player session over a lossy network. M9 task 6.1.

#include "net.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/replay/crash.h>

#include <cstring>
#include <utility>

namespace cy::mp {
namespace {

[[nodiscard]] Allocator& session_allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

template <class T, class... Args>
[[nodiscard]] T* make(Allocator& allocator, Args&&... args) noexcept {
    void* storage = allocator.allocate(sizeof(T), alignof(T));
    if (storage == nullptr) {
        return nullptr;
    }
    return construct_at<T>(storage, std::forward<Args>(args)...);
}

template <class T>
void unmake(Allocator& allocator, T* object) noexcept {
    if (object == nullptr) {
        return;
    }
    object->~T();
    allocator.deallocate(object, sizeof(T), alignof(T));
}

/// The wire. Two packet kinds, each with its own tag, because a receiver that told them apart by
/// size would tell them apart wrongly the day one of them grew a field.
constexpr u32 kInputTag = 0x4D50'0001U;
constexpr u32 kAuthorityTag = 0x4D50'0002U;
constexpr net::ChannelId kInputChannel = 0;
constexpr net::ChannelId kAuthorityChannel = 1;

struct InputPacket {
    u32 tag = kInputTag;
    u32 player = 0;
    u64 tick = 0;
    i32 dx = 0;
    i32 dy = 0;
};

struct AuthorityPacket {
    u32 tag = kAuthorityTag;
    u32 substituted_mask = 0;
    u64 tick = 0;
    i32 dx[kPlayers] = {};
    i32 dy[kPlayers] = {};
};

template <class T>
[[nodiscard]] bool decode(Span<const u8> bytes, u32 tag, T& out) noexcept {
    if (bytes.size() != sizeof(T)) {
        return false;
    }
    std::memcpy(static_cast<void*>(&out), bytes.data(), sizeof(T));
    return out.tag == tag;
}

template <class T>
[[nodiscard]] Span<const u8> encode(const T& value) noexcept {
    return {reinterpret_cast<const u8*>(&value), sizeof(T)};
}

[[nodiscard]] bool same_intent(const MoveIntent& left, const MoveIntent& right) noexcept {
    return left.dx == right.dx && left.dy == right.dy;
}

}  // namespace

u32 SessionReport::rollbacks() const noexcept {
    u32 total = 0;
    for (const ClientReport& client : clients) {
        total += client.rollbacks;
    }
    return total;
}

u32 SessionReport::duplicate_effects() const noexcept {
    u32 total = 0;
    for (const ClientReport& client : clients) {
        total += client.duplicate_effects;
    }
    return total;
}

u32 SessionReport::converged_clients() const noexcept {
    u32 total = 0;
    for (const ClientReport& client : clients) {
        total += client.converged ? 1U : 0U;
    }
    return total;
}

// ================================================================================================
// THE CLIENT
// ================================================================================================

/// One player's machine: a world it predicts into, the authoritative commands it has been told
/// about, and the rollback engine that reconciles the two.
class NetworkedSession::Client {
public:
    Client(u32 index, const SessionOptions& options, const replay::CompatibilityManifest& manifest,
           net::LocalTransport& transport, net::PeerId host) noexcept
        : index_(index),
          options_(options),
          transport_(&transport),
          host_(host),
          log_(session_allocator(), manifest),
          ring_(session_allocator(), 24ULL * 1024 * 1024),
          ledger_(session_allocator()),
          engine_(session_allocator(), log_, ring_, ledger_, epochs_),
          predicted_(session_allocator()),
          played_(session_allocator()),
          authoritative_(session_allocator()) {}

    [[nodiscard]] bool build() noexcept;
    [[nodiscard]] bool send_input(u64 tick) noexcept;
    /// Take everything the host has told us. Sets `mispredicted_from_` when the truth disagrees
    /// with what we simulated.
    [[nodiscard]] bool receive() noexcept;
    /// Roll back to the earliest mispredicted tick and re-simulate to `through`.
    [[nodiscard]] bool reconcile(u64 through) noexcept;
    /// Predict this tick and run it.
    [[nodiscard]] bool simulate(u64 tick) noexcept;

    [[nodiscard]] GameWorld& world() noexcept { return world_; }
    [[nodiscard]] u32 rollbacks_so_far() const noexcept { return report_.rollbacks; }
    [[nodiscard]] u32 resimulated_so_far() const noexcept { return report_.ticks_resimulated; }
    [[nodiscard]] const ClientReport& finish(u64 host_hash) noexcept;

private:
    struct Played {
        u64 instance = 0;
        u64 tick = 0;
    };

    [[nodiscard]] bool predict_into(u64 tick) noexcept;
    [[nodiscard]] MoveIntent* slot(u64 tick) noexcept;
    [[nodiscard]] bool absorb(const AuthorityPacket& packet) noexcept;
    void note_effect(u64 instance) noexcept;

    static Status feed(void* user, u64 tick, Span<const replay::LogRecord> commands) noexcept;
    static Status step(void* user, u64 tick) noexcept;
    static void on_effect(void* user, u64 kind, u64 instance) noexcept;

    u32 index_;
    SessionOptions options_;
    net::LocalTransport* transport_;
    net::PeerId host_;

    GameWorld world_;
    replay::RecordLog log_;
    replay::SnapshotRing ring_;
    replay::SideEffectLedger ledger_;
    determinism::EpochCounter epochs_;
    replay::RollbackEngine engine_;

    /// What we simulated, per (tick, player). The comparison the reconciliation is made of.
    Array<MoveIntent> predicted_;
    /// Every explosion this client actually played. The duplicate check reads it — see net.h.
    Array<Played> played_;
    /// Per tick: whether the input the world currently holds for it came from the host rather than
    /// from this client's prediction. What `ClientReport::first_unconfirmed_tick` is read off.
    Array<u8> authoritative_;

    MoveIntent last_authoritative_[kPlayers] = {};
    u64 frontier_ = 0;
    u64 last_simulated_ = 0;
    bool has_simulated_ = false;
    u64 mispredicted_from_ = 0;
    bool mispredicted_ = false;
    ClientReport report_;
};

bool NetworkedSession::Client::build() noexcept {
    if (!world_.build()) {
        return false;
    }
    if (!predicted_.resize((options_.ticks + 2) * kPlayers).has_value() ||
        !authoritative_.resize(options_.ticks + 2).has_value()) {
        return false;
    }
    if (!ledger_
             .declare(replay::EffectDeclaration{kExplosionKind, "explosion",
                                                replay::Speculation::Speculative,
                                                replay::Reconciliation::Cancel})
             .has_value()) {
        return false;
    }
    world_.set_effect_sink(&Client::on_effect, this);
    return true;
}

MoveIntent* NetworkedSession::Client::slot(u64 tick) noexcept {
    const usize offset = static_cast<usize>(tick) * kPlayers;
    if (offset + kPlayers > predicted_.size()) {
        return nullptr;
    }
    return predicted_.data() + offset;
}

bool NetworkedSession::Client::send_input(u64 tick) noexcept {
    const MoveIntent intent = intent_of(tick, index_);
    InputPacket packet;
    packet.player = index_;
    packet.tick = tick;
    packet.dx = intent.dx;
    packet.dy = intent.dy;
    // UNRELIABLE, AND THAT IS THE DECISION. An input retransmitted after its tick has been
    // simulated arrives as a correction, not as an input — see net.h.
    return transport_->send(host_, kInputChannel, net::DeliveryMode::Unreliable, encode(packet))
        .has_value();
}

bool NetworkedSession::Client::absorb(const AuthorityPacket& packet) noexcept {
    MoveIntent* predicted = slot(packet.tick);
    if (predicted == nullptr) {
        return false;
    }
    bool differs = false;
    for (u32 player = 0; player < kPlayers; ++player) {
        const MoveIntent truth{packet.dx[player], packet.dy[player]};
        differs = differs || !same_intent(truth, predicted[player]);
        last_authoritative_[player] = truth;
        // The authoritative record, in this client's own identifiers. It is the same command the
        // host committed; what is not the same is the world it addresses, and a record built from
        // the host's entity ids would only work because both worlds happen to allocate alike.
        if (!log_.append(world_.command_record(player, packet.tick, truth)).has_value()) {
            return false;
        }
    }
    frontier_ = packet.tick + 1;

    // A TICK WE HAVE ALREADY SIMULATED, SIMULATED WRONGLY. Remember the EARLIEST such tick: a
    // rollback to the second of two mispredictions would leave the first one in the world.
    if (has_simulated_ && packet.tick <= last_simulated_ && differs) {
        ++report_.mispredicted_ticks;
        if (!mispredicted_ || packet.tick < mispredicted_from_) {
            mispredicted_from_ = packet.tick;
        }
        mispredicted_ = true;
    }
    return true;
}

bool NetworkedSession::Client::receive() noexcept {
    net::Datagram datagram;
    while (transport_->receive(datagram)) {
        AuthorityPacket packet;
        if (datagram.channel != kAuthorityChannel ||
            !decode(datagram.bytes, kAuthorityTag, packet)) {
            continue;
        }
        if (!absorb(packet)) {
            return false;
        }
    }
    return true;
}

bool NetworkedSession::Client::predict_into(u64 tick) noexcept {
    MoveIntent* predicted = slot(tick);
    if (predicted == nullptr) {
        return false;
    }
    for (u32 player = 0; player < kPlayers; ++player) {
        // ITS OWN INPUT EXACTLY; EVERY REMOTE PLAYER BY REPEATING WHAT IT LAST HEARD. That is the
        // whole of the prediction, and every rollback below is one of its mistakes being corrected.
        const MoveIntent intent =
            (player == index_) ? intent_of(tick, player) : last_authoritative_[player];
        predicted[player] = intent;
        if (!world_.offer_intent(player, tick, intent)) {
            return false;
        }
    }
    return true;
}

Status NetworkedSession::Client::feed(void* user, u64 tick,
                                      Span<const replay::LogRecord> commands) noexcept {
    auto* self = static_cast<Client*>(user);
    // RE-CAPTURE AS WE GO, and this is not an optimisation. The ring's capture for tick T was taken
    // during the run that has just been shown to be wrong; a later rollback to a tick inside this
    // window would restore that stale state and reconcile onto a world that never existed. The
    // capture is taken here rather than in `step` because `feed` runs BEFORE the tick, which is the
    // convention `rollback.h` writes down.
    if (Status captured =
            self->ring_.capture(self->world_.world(), self->world_.providers(),
                                determinism::SimulationPoint{self->epochs_.current(), tick});
        !captured) {
        return captured;
    }
    if (tick < self->authoritative_.size()) {
        self->authoritative_[tick] = commands.empty() ? 0U : 1U;
    }
    if (!commands.empty()) {
        return self->world_.feed(commands)
                   ? ok()
                   : fail(ErrorCode::Unknown, "the client refused a recorded command");
    }
    // Past the authoritative frontier: predict again, with whatever we have learned since.
    return self->predict_into(tick) ? ok()
                                    : fail(ErrorCode::Unknown, "the client could not predict");
}

Status NetworkedSession::Client::step(void* user, u64 tick) noexcept {
    auto* self = static_cast<Client*>(user);
    return self->world_.step(tick) ? ok() : fail(ErrorCode::Unknown, "the client refused to step");
}

void NetworkedSession::Client::note_effect(u64 instance) noexcept {
    const u64 tick = engine_.point().tick;
    for (const Played& entry : played_.span()) {
        if (entry.instance == instance && entry.tick == tick) {
            // **THE EXPLOSION THAT PLAYED TWICE.** Counted here rather than inferred from the
            // ledger's own numbers, so that removing the ledger's decision is visible in this
            // program's output rather than only in a unit test.
            ++report_.duplicate_effects;
            return;
        }
    }
    (void)played_.push_back(Played{instance, tick});
    ++report_.explosions_played;
}

void NetworkedSession::Client::on_effect(void* user, u64 kind, u64 instance) noexcept {
    auto* self = static_cast<Client*>(user);
    const bool predicted = self->engine_.point().tick >= self->frontier_;
    switch (self->engine_.offer(kind, instance, predicted)) {
        case replay::EffectVerdict::Realise:
            self->note_effect(instance);
            break;
        case replay::EffectVerdict::SuppressedAlreadyRealised:
        case replay::EffectVerdict::DeferredUntilConfirmed:
        case replay::EffectVerdict::Undeclared:
            break;
    }
}

bool NetworkedSession::Client::simulate(u64 tick) noexcept {
    if (!ring_
             .capture(world_.world(), world_.providers(),
                      determinism::SimulationPoint{epochs_.current(), tick})
             .has_value()) {
        return false;
    }
    engine_.set_point(determinism::SimulationPoint{epochs_.current(), tick});
    if (tick < authoritative_.size()) {
        authoritative_[tick] = 0;
    }
    if (!predict_into(tick) || !world_.step(tick)) {
        return false;
    }
    last_simulated_ = tick;
    has_simulated_ = true;
    return true;
}

bool NetworkedSession::Client::reconcile(u64 through) noexcept {
    if (!mispredicted_ || !has_simulated_) {
        return true;
    }
    const u64 from = mispredicted_from_;
    mispredicted_ = false;
    if (from > through) {
        return true;
    }
    replay::RollbackHooks hooks{&Client::feed, &Client::step, this};
    replay::RollbackReport rollback;
    if (!engine_.roll_back(from, through, world_.world(), world_.providers(), hooks, rollback)
             .has_value()) {
        return false;
    }
    if (!rollback.performed) {
        ++report_.refusals;
        return true;
    }
    ++report_.rollbacks;
    report_.ticks_resimulated += rollback.ticks_resimulated;
    return true;
}

const ClientReport& NetworkedSession::Client::finish(u64 host_hash) noexcept {
    report_.first_unconfirmed_tick = options_.ticks;
    for (u64 tick = 0; tick < options_.ticks; ++tick) {
        if (authoritative_[tick] == 0) {
            report_.first_unconfirmed_tick = tick;
            break;
        }
    }
    report_.frontier = frontier_;
    report_.effects_offered = static_cast<u32>(engine_.effects_offered());
    report_.effects_suppressed = static_cast<u32>(engine_.effects_suppressed());
    report_.final_hash = world_.root_hash();
    report_.converged = report_.final_hash == host_hash;
    return report_;
}

// ================================================================================================
// THE SESSION
// ================================================================================================

NetworkedSession::NetworkedSession(const SessionOptions& options,
                                   const replay::CompatibilityManifest& manifest) noexcept
    : options_(options),
      manifest_(manifest),
      network_(session_allocator(), options.seed),
      client_transports_(session_allocator()),
      clients_(session_allocator()),
      log_(session_allocator(), manifest),
      recorder_(log_),
      checkpoints_(session_allocator(), 32ULL * 1024 * 1024),
      ring_(session_allocator()),
      host_ledger_(session_allocator()),
      host_engine_(session_allocator(), log_, checkpoints_, host_ledger_, host_epochs_),
      arrived_(session_allocator()),
      arrived_flag_(session_allocator()),
      trace_(session_allocator()) {}

NetworkedSession::~NetworkedSession() {
    if (built_) {
        replay::SessionRecorder::detach(host_world_.commands());
    }
    for (Client* client : clients_.span()) {
        unmake(session_allocator(), client);
    }
    for (net::LocalTransport* transport : client_transports_.span()) {
        unmake(session_allocator(), transport);
    }
    unmake(session_allocator(), host_transport_);
}

bool NetworkedSession::link() noexcept {
    for (u32 player = 0; player < kPlayers; ++player) {
        net::LocalTransport* transport = client_transports_[player];
        if (!transport->connect("host").has_value() ||
            !host_transport_->accept(client_ids_[player]).has_value()) {
            return false;
        }
    }
    return true;
}

bool NetworkedSession::build() noexcept {
    net::NetworkConditions conditions;
    conditions.latency_ms = options_.latency_ms;
    conditions.jitter_ms = options_.jitter_ms;
    conditions.loss_percent = options_.loss_percent;
    conditions.duplication_percent = options_.duplication_percent;
    conditions.reorder_percent = options_.reorder_percent;
    network_.set_conditions(conditions);

    auto host = network_.add_host("host");
    if (!host) {
        return false;
    }
    host_id_ = *host;
    host_transport_ =
        make<net::LocalTransport>(session_allocator(), session_allocator(), network_, host_id_);
    if (host_transport_ == nullptr) {
        return false;
    }

    static const char* const kNames[kPlayers] = {"player-0", "player-1", "player-2", "player-3"};
    for (u32 player = 0; player < kPlayers; ++player) {
        auto peer = network_.add_host(kNames[player]);
        if (!peer) {
            return false;
        }
        client_ids_[player] = *peer;
        auto* transport =
            make<net::LocalTransport>(session_allocator(), session_allocator(), network_, *peer);
        if (transport == nullptr || !client_transports_.push_back(transport)) {
            return false;
        }
        auto* client =
            make<Client>(session_allocator(), player, options_, manifest_, *transport, host_id_);
        if (client == nullptr || !clients_.push_back(client) || !client->build()) {
            return false;
        }
    }
    if (!link() || !host_world_.build()) {
        return false;
    }

    if (!arrived_.resize((options_.ticks + 2) * kPlayers).has_value() ||
        !arrived_flag_.resize((options_.ticks + 2) * kPlayers).has_value()) {
        return false;
    }
    if (!host_ledger_
             .declare(replay::EffectDeclaration{kExplosionKind, "explosion",
                                                replay::Speculation::Speculative,
                                                replay::Reconciliation::Cancel})
             .has_value()) {
        return false;
    }
    host_world_.set_effect_sink(&NetworkedSession::on_host_effect, this);

    // THE CRASH REPLAY BUFFER. Its size is computed from the session's own rate and record volume
    // rather than chosen — `crash_ring_records()` returns zero for a window it cannot serve, and
    // zero means "do not enable the buffer" rather than "use a default".
    const u32 records =
        replay::crash_ring_records(options_.crash_window_seconds, manifest_.tick_rate_numerator,
                                   manifest_.tick_rate_denominator, kPlayers + 2);
    if (records != 0) {
        if (!ring_.reserve(records).has_value()) {
            return false;
        }
        recorder_.mirror_to(&ring_);
    }
    recorder_.attach(host_world_.commands());
    built_ = true;
    return true;
}

void NetworkedSession::on_host_effect(void* user, u64 kind, u64 instance) noexcept {
    auto* self = static_cast<NetworkedSession*>(user);
    if (self->host_engine_.offer(kind, instance, /*predicted=*/false) ==
        replay::EffectVerdict::Realise) {
        ++self->host_explosions_;
        (void)self->recorder_.record_effect(kind, instance);
    }
}

void NetworkedSession::advance_transports(u64 now_ms) noexcept {
    network_.advance(now_ms);
    host_transport_->advance(now_ms);
    for (net::LocalTransport* transport : client_transports_.span()) {
        transport->advance(now_ms);
    }
}

bool NetworkedSession::send_inputs(u64 wall_tick) noexcept {
    for (Client* client : clients_.span()) {
        if (!client->send_input(wall_tick)) {
            return false;
        }
    }
    return true;
}

bool NetworkedSession::drain_host() noexcept {
    net::Datagram datagram;
    while (host_transport_->receive(datagram)) {
        InputPacket packet;
        if (datagram.channel != kInputChannel || !decode(datagram.bytes, kInputTag, packet)) {
            continue;
        }
        const usize offset = (static_cast<usize>(packet.tick) * kPlayers) + packet.player;
        if (packet.player >= kPlayers || offset >= arrived_.size()) {
            continue;
        }
        arrived_[offset] = MoveIntent{packet.dx, packet.dy};
        arrived_flag_[offset] = 1;
    }
    return true;
}

bool NetworkedSession::host_tick(u64 simulated) noexcept {
    substituted_mask_ = 0;
    for (u32 player = 0; player < kPlayers; ++player) {
        const usize offset = (static_cast<usize>(simulated) * kPlayers) + player;
        if (arrived_flag_[offset] != 0) {
            decided_[player] = arrived_[offset];
            last_known_[player] = decided_[player];
        } else {
            // THE DEADLINE PASSED WITH NOTHING. `LatePolicy::RepeatPrevious`, and the substituted
            // input is recorded as an ordinary command — so a replay reproduces the substitution by
            // replaying it rather than by re-running the decision that produced it.
            decided_[player] = last_known_[player];
            ++report_.substitutions;
            substituted_mask_ |= 1U << player;
        }
        if (!host_world_.offer_intent(player, simulated, decided_[player])) {
            return false;
        }
    }

    const bool checkpoint = (simulated % options_.checkpoint_every) == 0;
    if (checkpoint && !checkpoints_
                           .capture(host_world_.world(), host_world_.providers(),
                                    determinism::SimulationPoint{host_epochs_.current(), simulated})
                           .has_value()) {
        return false;
    }
    recorder_.set_point(determinism::SimulationPoint{host_epochs_.current(), simulated});
    host_engine_.set_point(determinism::SimulationPoint{host_epochs_.current(), simulated});
    if (!host_world_.step(simulated)) {
        return false;
    }
    if (checkpoint) {
        if (!recorder_.record_checkpoint(simulated).has_value()) {
            return false;
        }
        ++report_.checkpoints;
    }
    const u64 hash = host_world_.root_hash();
    if (!recorder_.record_state_hash(hash).has_value()) {
        return false;
    }

    pending_ = TraceSample{};
    pending_.tick = simulated;
    for (u32 player = 0; player < kPlayers; ++player) {
        pending_.x[player] = host_world_.position_of(player).x;
        pending_.y[player] = host_world_.position_of(player).y;
        pending_.rollbacks[player] = static_cast<u16>(clients_[player]->rollbacks_so_far());
        pending_.depth[player] = static_cast<u16>(clients_[player]->resimulated_so_far());
    }
    pending_.substituted = substituted_mask_;
    pending_.host_hash = hash;
    last_tick_ = simulated;
    return true;
}

bool NetworkedSession::close_tick() noexcept {
    // The DELTA, not the total: `host_tick` stamped each client's counters before the clients ran,
    // so what is left here is what this tick's reconciliation cost.
    for (u32 player = 0; player < kPlayers; ++player) {
        pending_.rollbacks[player] =
            static_cast<u16>(clients_[player]->rollbacks_so_far() - pending_.rollbacks[player]);
        pending_.depth[player] =
            static_cast<u16>(clients_[player]->resimulated_so_far() - pending_.depth[player]);
    }
    return trace_.push_back(pending_).has_value();
}

bool NetworkedSession::broadcast(u64 simulated) noexcept {
    AuthorityPacket packet;
    packet.tick = simulated;
    packet.substituted_mask = substituted_mask_;
    for (u32 player = 0; player < kPlayers; ++player) {
        packet.dx[player] = decided_[player].dx;
        packet.dy[player] = decided_[player].dy;
    }
    u32 sent = 0;
    for (const net::PeerId client : client_ids_) {
        // RELIABLE ORDERED: a client that never learned the truth would never reconcile, and
        // "it diverged" and "it never found out" must not look alike.
        if (host_transport_
                ->send(client, kAuthorityChannel, net::DeliveryMode::ReliableOrdered,
                       encode(packet))
                .has_value()) {
            ++sent;
        }
    }
    return sent == kPlayers;
}

bool NetworkedSession::run() noexcept {
    const u64 delay = options_.input_delay_ticks;
    // The wall clock runs past the last simulated tick so that every authoritative packet in flight
    // is delivered and every outstanding reconciliation happens. A session that stopped the clock
    // at the last tick would report clients that had simply not finished.
    // TWO SECONDS OF WALL CLOCK PAST THE LAST TICK. The authoritative broadcast is reliable, so a
    // packet the network destroyed near the end has to be retransmitted and acknowledged before the
    // last client can reconcile — and at 8 % loss the last few ticks are exactly the ones most
    // likely to need it. A shorter drain reported clients that had simply not finished, which reads
    // identically to clients that diverged and is the failure this artefact exists to tell apart.
    const u64 drain = delay + 120;
    for (u64 wall = 0; wall < options_.ticks + delay + drain; ++wall) {
        const u64 now_ms = wall * kTickMillis;
        if (wall < options_.ticks && !send_inputs(wall)) {
            return false;
        }
        advance_transports(now_ms);
        if (!drain_host()) {
            return false;
        }

        const bool simulating = wall >= delay && (wall - delay) < options_.ticks;
        const u64 simulated = simulating ? wall - delay : 0;
        if (simulating && (!host_tick(simulated) || !broadcast(simulated))) {
            return false;
        }

        for (Client* client : clients_.span()) {
            if (!client->receive()) {
                return false;
            }
            if (simulating) {
                if (simulated > 0 && !client->reconcile(simulated - 1)) {
                    return false;
                }
                if (!client->simulate(simulated)) {
                    return false;
                }
            } else if (!client->reconcile(last_tick_)) {
                return false;
            }
        }
        if (simulating && !close_tick()) {
            return false;
        }
    }
    replay::SessionRecorder::detach(host_world_.commands());
    built_ = false;

    report_.ticks_simulated = options_.ticks;
    report_.datagrams_offered = network_.offered();
    report_.datagrams_dropped = network_.dropped();
    report_.datagrams_duplicated = network_.duplicated();
    report_.datagrams_delivered = network_.delivered();
    report_.host_explosions = host_explosions_;
    report_.host_final_hash = host_world_.root_hash();
    report_.records_written = static_cast<u32>(recorder_.records_written());
    report_.records_dropped = recorder_.dropped();
    report_.crash_ring_capacity = ring_.capacity();
    report_.crash_ring_size = ring_.size();
    report_.crash_ring_overwritten = ring_.overwritten();
    for (u32 player = 0; player < kPlayers; ++player) {
        report_.clients[player] = clients_[player]->finish(report_.host_final_hash);
    }
    return true;
}

}  // namespace cy::mp
