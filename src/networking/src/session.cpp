// SPDX-License-Identifier: MIT
#include <cy/networking/session.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace cy::net {
namespace {

void put_u16(Array<u8>& out, u16 value) noexcept {
    (void)out.push_back(static_cast<u8>(value));
    (void)out.push_back(static_cast<u8>(value >> 8));
}

void put_u32(Array<u8>& out, u32 value) noexcept {
    for (u32 index = 0; index < 4; ++index) {
        (void)out.push_back(static_cast<u8>(value >> (index * 8)));
    }
}

void put_u64(Array<u8>& out, u64 value) noexcept {
    for (u32 index = 0; index < 8; ++index) {
        (void)out.push_back(static_cast<u8>(value >> (index * 8)));
    }
}

/// Reads little-endian fields from a message, refusing to read past its end.
class Cursor {
public:
    explicit Cursor(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool u8_(u8& out) noexcept {
        if (offset_ + 1 > bytes_.size()) {
            return false;
        }
        out = bytes_[offset_++];
        return true;
    }
    [[nodiscard]] bool u16_(u16& out) noexcept { return integer(out, 2); }
    [[nodiscard]] bool u32_(u32& out) noexcept { return integer(out, 4); }
    [[nodiscard]] bool u64_(u64& out) noexcept { return integer(out, 8); }
    [[nodiscard]] bool bytes(u8* out, usize count) noexcept {
        if (offset_ + count > bytes_.size()) {
            return false;
        }
        std::memcpy(out, bytes_.data() + offset_, count);
        offset_ += count;
        return true;
    }
    [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }

private:
    template <class T>
    [[nodiscard]] bool integer(T& out, u32 size) noexcept {
        if (offset_ + size > bytes_.size()) {
            return false;
        }
        u64 value = 0;
        for (u32 index = 0; index < size; ++index) {
            value |= static_cast<u64>(bytes_[offset_ + index]) << (index * 8);
        }
        offset_ += size;
        out = static_cast<T>(value);
        return true;
    }

    Span<const u8> bytes_;
    usize offset_ = 0;
};

constexpr u32 kMaxScopeText = 31;

[[nodiscard]] Status encode_scope(const CompatibilityScope& scope, Array<u8>& out) noexcept {
    out.clear();
    (void)out.push_back(static_cast<u8>(SessionMessage::Join));
    put_u64(out, scope.build_id);
    put_u64(out, scope.schema_set_hash);
    (void)out.push_back(static_cast<u8>(scope.profile));
    for (const char* text : {scope.platform, scope.architecture}) {
        const usize length = std::strlen(text);
        if (length > kMaxScopeText) {
            return fail(ErrorCode::OutOfRange, "networking: a platform name longer than 31 bytes");
        }
        (void)out.push_back(static_cast<u8>(length));
        if (Status appended = out.append(Span<const u8>(reinterpret_cast<const u8*>(text), length));
            !appended) {
            return appended;
        }
    }
    return ok();
}

[[nodiscard]] bool read_text(Cursor& cursor, char (&out)[kMaxScopeText + 1]) noexcept {
    u8 length = 0;
    if (!cursor.u8_(length) || length > kMaxScopeText) {
        return false;
    }
    if (!cursor.bytes(reinterpret_cast<u8*>(out), length)) {
        return false;
    }
    out[length] = '\0';
    return true;
}

constexpr u64 kFnvOffset = 0xCBF2'9CE4'8422'2325ULL;
constexpr u64 kFnvPrime = 0x0000'0100'0000'01B3ULL;

void fold(u64& hash, u64 value, u32 bytes) noexcept {
    for (u32 index = 0; index < bytes; ++index) {
        hash = (hash ^ ((value >> (index * 8)) & 0xFFU)) * kFnvPrime;
    }
}

}  // namespace

// --- Commands on the wire ------------------------------------------------------------------------

Status encode_commands(Span<const NetCommand> commands, Array<u8>& out) noexcept {
    out.clear();
    if (commands.size() > 0xFFFFU) {
        return fail(ErrorCode::OutOfRange, "networking: too many commands for one message");
    }
    (void)out.push_back(static_cast<u8>(SessionMessage::Commands));
    put_u16(out, static_cast<u16>(commands.size()));
    for (const NetCommand& command : commands) {
        if (command.payload_size > kMaxNetCommandPayload) {
            return fail(ErrorCode::OutOfRange, "networking: a command payload over 48 bytes");
        }
        put_u64(out, command.tick);
        put_u32(out, command.type);
        put_u64(out, command.target.value());
        (void)out.push_back(static_cast<u8>(command.payload_size));
        if (Status appended = out.append(Span<const u8>(command.payload, command.payload_size));
            !appended) {
            return appended;
        }
    }
    return ok();
}

Status decode_commands(Span<const u8> bytes, Array<NetCommand>& out) noexcept {
    out.clear();
    Cursor cursor(bytes);
    u8 type = 0;
    u16 count = 0;
    if (!cursor.u8_(type) || type != static_cast<u8>(SessionMessage::Commands) ||
        !cursor.u16_(count)) {
        return fail(ErrorCode::InvalidArgument, "networking: not a commands message");
    }
    for (u32 index = 0; index < count; ++index) {
        NetCommand command;
        u64 target = 0;
        u8 size = 0;
        if (!cursor.u64_(command.tick) || !cursor.u32_(command.type) || !cursor.u64_(target) ||
            !cursor.u8_(size) || size > kMaxNetCommandPayload ||
            !cursor.bytes(command.payload, size)) {
            return fail(ErrorCode::InvalidArgument, "networking: a command cut short or oversized");
        }
        command.target = NetworkId::make(static_cast<u16>(target >> 48), target);
        command.payload_size = size;
        if (Status pushed = out.push_back(command); !pushed) {
            return pushed;
        }
    }
    if (!cursor.done()) {
        return fail(ErrorCode::InvalidArgument, "networking: bytes after the last command");
    }
    return ok();
}

u64 ServerInputLog::hash() const noexcept {
    u64 hash = kFnvOffset;
    for (const AppliedCommand& entry : entries_) {
        fold(hash, static_cast<u64>(entry.kind), 1);
        fold(hash, entry.tick, 8);
        fold(hash, entry.peer.value(), 8);
        fold(hash, entry.command.tick, 8);
        fold(hash, entry.command.type, 4);
        fold(hash, entry.command.target.value(), 8);
        fold(hash, entry.command.payload_size, 2);
        for (u32 index = 0; index < entry.command.payload_size; ++index) {
            fold(hash, entry.command.payload[index], 1);
        }
    }
    return hash;
}

// --- ServerHost ----------------------------------------------------------------------------------

struct ServerHost::Slot {
    PeerId peer;
    ServerPeerStats stats{};
    /// The scope's strings live here: `DedicatedServer` keeps the pointers.
    char platform[kMaxScopeText + 1] = {};
    char architecture[kMaxScopeText + 1] = {};
    /// The newest tick this peer issued a command for, and how many it issued for it.
    u64 issued_tick = 0;
    u32 issued_count = 0;
    bool issued_any = false;
};

ServerHost::ServerHost(Allocator& allocator, Transport& transport, ecs::World& world,
                       WorldReplicator& replicator, const ServerHostConfig& config) noexcept
    : allocator_(&allocator),
      transport_(&transport),
      world_(&world),
      replicator_(&replicator),
      config_(config),
      admission_(allocator, transport, config.mode, config.scope, config.maximum_peers),
      slots_(allocator),
      pending_(allocator),
      membership_(allocator),
      due_(allocator),
      farewells_(allocator),
      log_(allocator),
      message_(allocator) {
    if (config_.tick_rate_hz == 0) {
        config_.tick_rate_hz = 30;
    }
    if (config_.snapshot_interval_ticks == 0) {
        config_.snapshot_interval_ticks = 1;
    }
}

ServerHost::~ServerHost() {
    for (Slot* slot : slots_) {
        slot->~Slot();
        allocator_->deallocate(slot, sizeof(Slot), alignof(Slot));
    }
}

ServerHost::Slot* ServerHost::find(PeerId peer) noexcept {
    for (Slot* slot : slots_) {
        if (slot->peer == peer) {
            return slot;
        }
    }
    return nullptr;
}

u32 ServerHost::peer_count() const noexcept {
    return static_cast<u32>(slots_.size());
}

const ServerPeerStats* ServerHost::peer_stats(PeerId peer) const noexcept {
    for (const Slot* slot : slots_) {
        if (slot->peer == peer) {
            return &slot->stats;
        }
    }
    return nullptr;
}

void ServerHost::update(u64 now_us) noexcept {
    now_us_ = now_us;
    if (!started_) {
        started_ = true;
        next_tick_us_ = now_us;
    }
    transport_->advance_us(now_us);
    handle_events();
    Datagram datagram;
    while (transport_->receive(datagram)) {
        handle_message(datagram.peer, datagram.channel, datagram.bytes);
    }
    const u64 period = 1'000'000ULL / config_.tick_rate_hz;
    // A host that fell far behind — a debugger, a stalled machine — runs a bounded catch-up rather
    // than a burst of hundreds of ticks that would itself fall behind.
    if (now_us > next_tick_us_ + (period * 8)) {
        next_tick_us_ = now_us - (period * 8);
    }
    while (now_us >= next_tick_us_) {
        step();
        next_tick_us_ += period;
    }
    for (usize index = 0; index < farewells_.size();) {
        if (now_us >= farewells_[index].at_us) {
            transport_->disconnect(farewells_[index].peer);
            farewells_.remove_unordered(index);
            continue;
        }
        ++index;
    }
    transport_->advance_us(now_us);
}

void ServerHost::handle_events() noexcept {
    ConnectionEvent event;
    while (transport_->poll_event(event)) {
        if (event.kind == ConnectionEvent::Kind::Disconnected) {
            drop(event.peer);
        }
    }
}

void ServerHost::drop(PeerId peer) noexcept {
    for (usize index = 0; index < slots_.size(); ++index) {
        if (slots_[index]->peer != peer) {
            continue;
        }
        admission_.drop(peer);
        replicator_->remove_peer(peer);
        slots_[index]->~Slot();
        allocator_->deallocate(slots_[index], sizeof(Slot), alignof(Slot));
        slots_.remove_unordered(index);
        AppliedCommand left;
        left.kind = AppliedCommand::Kind::Left;
        left.peer = peer;
        (void)membership_.push_back(left);
        return;
    }
}

void ServerHost::handle_message(PeerId peer, ChannelId channel, Span<const u8> bytes) noexcept {
    if (bytes.empty()) {
        return;
    }
    const auto type = static_cast<SessionMessage>(bytes[0]);
    if (channel == session_channel::kControl && type == SessionMessage::Join) {
        handle_join(peer, bytes);
        return;
    }
    Slot* slot = find(peer);
    if (slot == nullptr) {
        // Nothing but a join is heard from a peer that has not been admitted.
        return;
    }
    if (channel == session_channel::kAcks && type == SessionMessage::Ack && bytes.size() == 9) {
        Cursor cursor(bytes.subspan(1));
        u32 newest = 0;
        u32 previous = 0;
        if (cursor.u32_(newest) && cursor.u32_(previous)) {
            replicator_->acknowledge(peer, newest, previous);
        }
        return;
    }
    if (channel == session_channel::kCommands && type == SessionMessage::Commands) {
        handle_commands(*slot, bytes);
    }
}

void ServerHost::send_refusal(PeerId peer, const AdmissionRejection& rejection) noexcept {
    message_.clear();
    (void)message_.push_back(static_cast<u8>(SessionMessage::JoinRefused));
    (void)message_.push_back(static_cast<u8>(rejection.refusal));
    (void)message_.push_back(static_cast<u8>(rejection.compatibility));
    (void)transport_->send(peer, session_channel::kControl, DeliveryMode::ReliableOrdered,
                           message_.span());
    Farewell farewell;
    farewell.peer = peer;
    farewell.at_us = now_us_ + 1'000'000;
    (void)farewells_.push_back(farewell);
    ++refused_joins_;
}

void ServerHost::handle_join(PeerId peer, Span<const u8> bytes) noexcept {
    if (find(peer) != nullptr) {
        return;
    }
    void* storage = allocator_->allocate(sizeof(Slot), alignof(Slot));
    if (storage == nullptr) {
        return;
    }
    Slot* slot = construct_at<Slot>(storage);
    slot->peer = peer;
    slot->stats.peer = peer;
    Cursor cursor(bytes.subspan(1));
    CompatibilityScope scope;
    u8 profile = 0;
    const bool parsed = cursor.u64_(scope.build_id) && cursor.u64_(scope.schema_set_hash) &&
                        cursor.u8_(profile) && read_text(cursor, slot->platform) &&
                        read_text(cursor, slot->architecture) && cursor.done();
    scope.profile = static_cast<determinism::DeterminismProfile>(profile);
    scope.platform = slot->platform;
    scope.architecture = slot->architecture;
    Expected<u32, AdmissionRejection> admitted =
        parsed ? admission_.admit(peer, scope, tick_)
               : Expected<u32, AdmissionRejection>(make_unexpected(
                     AdmissionRejection{AdmissionRefusal::Incompatible, JoinRefusal::None}));
    if (!admitted || !replicator_->add_peer(peer) || !slots_.push_back(slot)) {
        const AdmissionRejection rejection =
            admitted ? AdmissionRejection{AdmissionRefusal::SessionFull, JoinRefusal::None}
                     : admitted.error();
        if (admitted) {
            admission_.drop(peer);
            replicator_->remove_peer(peer);
        }
        slot->~Slot();
        allocator_->deallocate(slot, sizeof(Slot), alignof(Slot));
        send_refusal(peer, rejection);
        return;
    }
    message_.clear();
    (void)message_.push_back(static_cast<u8>(SessionMessage::JoinAccepted));
    put_u32(message_, peer.slot());
    put_u32(message_, peer.generation());
    put_u64(message_, tick_);
    put_u32(message_, config_.tick_rate_hz);
    (void)transport_->send(peer, session_channel::kControl, DeliveryMode::ReliableOrdered,
                           message_.span());
    AppliedCommand joined;
    joined.kind = AppliedCommand::Kind::Joined;
    joined.peer = peer;
    (void)membership_.push_back(joined);
}

bool ServerHost::admit_command(Slot& slot, const NetCommand& command) const noexcept {
    const u64 distance = command.tick > tick_ ? command.tick - tick_ : tick_ - command.tick;
    if (distance > config_.command_window_ticks) {
        // A client claiming to act a minute in the future or the past is not a client with a slow
        // connection.
        return false;
    }
    // One ordered channel carries a peer's commands in the order it issued them, so the tick a
    // command names never goes backwards: one that does was not issued by an honest client.
    if (slot.issued_any && command.tick < slot.issued_tick) {
        return false;
    }
    // `networking-and-replication` — "Malicious input rate": at most `max_commands_per_tick`
    // issued for any one tick. Counted by the tick the client issued them for, not the tick they
    // arrived at, so a burst that loss and retransmission bunched together is not mistaken for a
    // client sending too fast.
    return !slot.issued_any || command.tick != slot.issued_tick ||
           slot.issued_count < config_.max_commands_per_tick;
}

void ServerHost::handle_commands(Slot& slot, Span<const u8> bytes) noexcept {
    Array<NetCommand> commands(*allocator_);
    if (!decode_commands(bytes, commands)) {
        ++slot.stats.commands_refused;
        slot.stats.flagged = true;
        return;
    }
    for (const NetCommand& command : commands) {
        if (!admit_command(slot, command)) {
            ++slot.stats.commands_refused;
            slot.stats.flagged = true;
            continue;
        }
        if (!slot.issued_any || command.tick != slot.issued_tick) {
            slot.issued_any = true;
            slot.issued_tick = command.tick;
            slot.issued_count = 0;
        }
        ++slot.issued_count;
        ++slot.stats.commands_accepted;
        Pending pending;
        pending.peer = slot.peer;
        pending.command = command;
        pending.arrival = arrivals_++;
        (void)pending_.push_back(pending);
    }
}

Status ServerHost::inject(PeerId peer, const NetCommand& command) noexcept {
    Pending pending;
    pending.peer = peer;
    pending.command = command;
    pending.arrival = arrivals_++;
    return pending_.push_back(pending);
}

void ServerHost::collect_due(Array<AppliedCommand>& out) noexcept {
    out.clear();
    for (AppliedCommand membership : membership_) {
        membership.tick = tick_;
        (void)out.push_back(membership);
    }
    membership_.clear();
    const usize first_command = out.size();
    usize kept = 0;
    for (const Pending& pending : pending_) {
        if (pending.command.tick > tick_) {
            pending_[kept++] = pending;
            continue;
        }
        AppliedCommand applied;
        applied.tick = tick_;
        applied.peer = pending.peer;
        applied.command = pending.command;
        applied.target = replicator_->entity_of(pending.command.target);
        (void)out.push_back(applied);
    }
    (void)pending_.resize(kept);
    // Stable, so arrival is the last key: it orders only one peer's commands for one tick, and
    // those arrive on one ordered channel in the order the client issued them.
    std::stable_sort(out.begin() + first_command, out.end(),
                     [](const AppliedCommand& a, const AppliedCommand& b) noexcept {
                         if (a.command.tick != b.command.tick) {
                             return a.command.tick < b.command.tick;
                         }
                         return a.peer.slot() < b.peer.slot();
                     });
}

void ServerHost::step() noexcept {
    ++tick_;
    collect_due(due_);
    run_step(due_.span());
}

void ServerHost::replay_step(Span<const AppliedCommand> entries) noexcept {
    ++tick_;
    due_.clear();
    for (AppliedCommand entry : entries) {
        entry.tick = tick_;
        if (entry.kind == AppliedCommand::Kind::Command) {
            entry.target = replicator_->entity_of(entry.command.target);
        }
        (void)due_.push_back(entry);
    }
    run_step(due_.span());
}

void ServerHost::run_step(Span<const AppliedCommand> due) noexcept {
    world_->advance_version();
    for (const AppliedCommand& entry : due) {
        (void)log_.append(entry);
        if (entry.kind != AppliedCommand::Kind::Command && config_.on_peer != nullptr) {
            config_.on_peer(config_.on_peer_user, entry.peer,
                            entry.kind == AppliedCommand::Kind::Joined);
        }
    }
    if (config_.simulate != nullptr) {
        config_.simulate(config_.simulate_user, *world_, tick_, due);
    }
    if (tick_ % config_.snapshot_interval_ticks == 0) {
        replicate();
    }
}

void ServerHost::replicate() noexcept {
    if (!replicator_->gather(tick_)) {
        return;
    }
    for (Slot* slot : slots_) {
        Expected<Span<const u8>, Error> built =
            replicator_->build(slot->peer, tick_, config_.budget);
        if (!built) {
            continue;
        }
        message_.clear();
        (void)message_.push_back(static_cast<u8>(SessionMessage::Snapshot));
        (void)message_.append(*built);
        if (transport_->send(slot->peer, session_channel::kSnapshots, DeliveryMode::Unreliable,
                             message_.span())) {
            ++slot->stats.snapshots_sent;
            slot->stats.snapshot_bytes += message_.size();
        }
    }
}

// --- ClientHost ----------------------------------------------------------------------------------

const char* client_phase_name(ClientPhase phase) noexcept {
    switch (phase) {
        case ClientPhase::Idle:
            return "Idle";
        case ClientPhase::Connecting:
            return "Connecting";
        case ClientPhase::Joining:
            return "Joining";
        case ClientPhase::Joined:
            return "Joined";
        case ClientPhase::Refused:
            return "Refused";
        case ClientPhase::Disconnected:
            return "Disconnected";
    }
    return "Unknown";
}

ClientHost::ClientHost(Allocator& allocator, Transport& transport, ReplicaWorld& replica,
                       const ClientHostConfig& config) noexcept
    : transport_(&transport),
      replica_(&replica),
      config_(config),
      outbox_(allocator),
      message_(allocator) {}

Status ClientHost::connect(const char* address) noexcept {
    Expected<PeerId, Error> peer = transport_->connect(address);
    if (!peer) {
        return make_unexpected(peer.error());
    }
    server_ = *peer;
    phase_ = ClientPhase::Connecting;
    return ok();
}

void ClientHost::disconnect() noexcept {
    if (server_.valid()) {
        transport_->disconnect(server_);
    }
    phase_ = ClientPhase::Disconnected;
}

Status ClientHost::submit(const NetCommand& command) noexcept {
    if (phase_ != ClientPhase::Joined) {
        return fail(ErrorCode::Unavailable, "networking: not joined to a session");
    }
    if (command.payload_size > kMaxNetCommandPayload) {
        return fail(ErrorCode::OutOfRange, "networking: a command payload over 48 bytes");
    }
    return outbox_.push_back(command);
}

void ClientHost::update(u64 now_us) noexcept {
    transport_->advance_us(now_us);
    handle_events();
    Datagram datagram;
    while (transport_->receive(datagram)) {
        if (datagram.peer == server_) {
            handle_message(datagram.channel, datagram.bytes);
        }
    }
    if (phase_ == ClientPhase::Joined) {
        send_ack();
        send_commands();
    }
    transport_->advance_us(now_us);
}

void ClientHost::handle_events() noexcept {
    ConnectionEvent event;
    while (transport_->poll_event(event)) {
        if (event.peer != server_) {
            continue;
        }
        if (event.kind == ConnectionEvent::Kind::Connected && phase_ == ClientPhase::Connecting) {
            phase_ = ClientPhase::Joining;
            send_join();
        } else if (event.kind == ConnectionEvent::Kind::Disconnected &&
                   phase_ != ClientPhase::Refused) {
            phase_ = ClientPhase::Disconnected;
        }
    }
}

void ClientHost::send_join() noexcept {
    if (encode_scope(config_.scope, message_)) {
        (void)transport_->send(server_, session_channel::kControl, DeliveryMode::ReliableOrdered,
                               message_.span());
    }
}

void ClientHost::handle_control(SessionMessage type, Span<const u8> body) noexcept {
    Cursor cursor(body);
    if (type == SessionMessage::JoinAccepted) {
        u32 slot = 0;
        u32 generation = 0;
        u32 rate = 0;
        if (cursor.u32_(slot) && cursor.u32_(generation) && cursor.u64_(server_tick_) &&
            cursor.u32_(rate)) {
            self_ = PeerId::make(slot, generation);
            phase_ = ClientPhase::Joined;
        }
        return;
    }
    if (type == SessionMessage::JoinRefused) {
        u8 refusal = 0;
        u8 compatibility = 0;
        if (cursor.u8_(refusal) && cursor.u8_(compatibility)) {
            refusal_.refusal = static_cast<AdmissionRefusal>(refusal);
            refusal_.compatibility = static_cast<JoinRefusal>(compatibility);
        }
        phase_ = ClientPhase::Refused;
        transport_->disconnect(server_);
    }
}

void ClientHost::handle_snapshot(Span<const u8> body) noexcept {
    Expected<SnapshotHeader, Error> applied = replica_->apply(body);
    // A stale snapshot comes back with its own number while `newest()` stays on the newer one.
    if (!applied || replica_->newest() != applied->snapshot) {
        return;
    }
    server_tick_ = applied->tick;
    ++applied_;
    snapshot_bytes_ += body.size() + 1;
    ack_due_ = true;
}

void ClientHost::handle_message(ChannelId channel, Span<const u8> bytes) noexcept {
    if (bytes.empty()) {
        return;
    }
    const auto type = static_cast<SessionMessage>(bytes[0]);
    if (channel == session_channel::kControl) {
        handle_control(type, bytes.subspan(1));
    } else if (channel == session_channel::kSnapshots && type == SessionMessage::Snapshot &&
               phase_ == ClientPhase::Joined) {
        handle_snapshot(bytes.subspan(1));
    }
}

void ClientHost::send_ack() noexcept {
    if (!ack_due_) {
        return;
    }
    ack_due_ = false;
    message_.clear();
    (void)message_.push_back(static_cast<u8>(SessionMessage::Ack));
    put_u32(message_, replica_->newest());
    put_u32(message_, replica_->previous_mask());
    (void)transport_->send(server_, session_channel::kAcks, DeliveryMode::Unreliable,
                           message_.span());
}

void ClientHost::send_commands() noexcept {
    if (outbox_.empty()) {
        return;
    }
    if (encode_commands(outbox_.span(), message_) &&
        transport_->send(server_, session_channel::kCommands, DeliveryMode::ReliableOrdered,
                         message_.span())) {
        outbox_.clear();
    }
}

}  // namespace cy::net
